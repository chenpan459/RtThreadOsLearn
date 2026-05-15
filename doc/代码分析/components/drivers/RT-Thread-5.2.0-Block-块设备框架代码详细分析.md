# RT-Thread 5.2.0 Block 块设备框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/block` 目录实现的**通用块设备框架**：物理盘 **`rt_blk_disk`**、逻辑分区 **`rt_blk_device`**、分区探测（EFI GPT / DFS 分区表）、以及与 DFS v2 配合的 **按字节访问 fops**。具体存储介质（SD、eMMC、NAND、AHCI 等）由 BSP 实现 **`struct rt_blk_disk_ops`** 后调用 **`rt_hw_blk_disk_register()`** 接入。

涉及路径：

- 核心：`blk.c`、`blk_dev.c`、`blk_partition.c`、`blk_dfs.c`
- 内部头文件：`blk_dev.h`、`blk_partition.h`、`blk_dfs.h`
- 分区实现子目录：`partitions/dfs.c`、`partitions/efi.c`、`partitions/efi.h`
- 对外 API：`rt-thread-5.2.0/components/drivers/include/drivers/blk.h`
- 块类公共定义：`components/drivers/include/drivers/classes/block.h`（`rtdevice.h` 在 `RT_USING_BLK` 下包含 `blk.h`，`classes/block.h` 由 `blk.h` 引入）
- 配置与构建：`Kconfig`、`SConscript`，以及 `partitions/Kconfig`、`partitions/SConscript`

---

## 1. 模块定位与对象模型

```text
BSP：实现 rt_blk_disk_ops（read/write/getgeome/...）
    ↓
rt_hw_blk_disk_register(rt_blk_disk *)
    ↓
本框架：注册整盘 rt_device（Class_Block）→ rt_blk_disk_probe_partition()
    ↓
可选：EFI GPT / DFS 分区表 → 为每个分区注册 rt_blk_device
    ↓
应用 / DFS：rt_device_read/write（扇区粒度）或（DFS v2）通过 fops 按字节访问
```

- **`struct rt_blk_disk`**（`blk.h`）：表示**整盘**物理设备，内嵌 **`struct rt_device parent`**，持有 **`ops`**、分区链表 **`part_nodes`**、自旋锁 **`lock`**、用户态互斥 **`usr_lock`**（信号量）、**`max_partitions`**、**`partitions`** 计数等。
- **`struct rt_blk_device`**：表示**单个分区**逻辑设备，同样 **`RT_Device_Class_Block`**，通过 **`sector_start` / `sector_count`** 把 I/O 映射到父盘的 LBA；含 **`dfs_partition`**（与 DFS 挂载元数据协作）、**`partno`**、链表节点挂入 **`disk->part_nodes`**。

宏 **`to_blk_disk` / `to_blk`**（`blk_dev.h`）由 `rt_container_of` 从 `struct rt_device *` 反查磁盘或分区对象。名称宏在 **`RT_USING_DM`** 下走 **`rt_dm_dev_get_name`**，否则使用 **`device->parent.name`**。

---

## 2. 公共控制命令与几何信息（`classes/block.h`）

| 宏 | 含义 |
|----|------|
| `RT_DEVICE_CTRL_BLK_GETGEOME` | 获取 **`struct rt_device_blk_geometry`**：`sector_count`、`bytes_per_sector`、`block_size`（擦除粒度等） |
| `RT_DEVICE_CTRL_BLK_SYNC` | 刷盘缓存 |
| `RT_DEVICE_CTRL_BLK_ERASE` | 整盘擦除类语义（由驱动实现） |
| `RT_DEVICE_CTRL_BLK_AUTOREFRESH` | 自动刷新模式开关 |
| `RT_DEVICE_CTRL_BLK_PARTITION` | 取分区描述（逻辑设备上有效） |

**`struct rt_device_blk_geometry`** 中 **`sector_count`** 为 **`rt_uint64_t`**，与旧代码中 32 位扇区计数相比更利于大容量盘。

**`blk_dfs.h`** 另定义 **`RT_DEVICE_CTRL_BLK_SSIZEGET`**、**`RT_DEVICE_CTRL_ALL_BLK_SSIZEGET`**（魔数常量），供 POSIX/DFS v2 层查询**扇区字节数**与**整盘总字节数**（见第 6 节）。

---

## 3. 磁盘层：`blk.c`

### 3.1 `blk_open` / `blk_close`

- **open**：若 **`disk->read_only`** 且以只写方式打开，返回 **`-RT_EINVAL`**。
- **close**：恒 **`RT_EOK`**。

### 3.2 读写在 `parallel_io` 下的差异

- **默认（`parallel_io == 0`）**：**`blk_read` / `blk_write`** 在调用 **`disk->ops->read/write`** 前后对 **`disk->usr_lock`** 做 **`rt_sem_take/release`**，多线程互斥访问同盘。
- **`parallel_io != 0`**：**`blk_parallel_read/write`** 不加信号量，由驱动或上层保证并发安全（例如底层已队列化）。

写路径若 **`read_only`**（未提供 **`ops->write`** 时注册阶段会置位），返回 **`-RT_ENOSYS`**。

### 3.3 `blk_control`

- **`RT_DEVICE_CTRL_BLK_GETGEOME`**：转 **`disk->ops->getgeome`**。
- **`RT_DEVICE_CTRL_BLK_SYNC`**：若存在 **`ops->sync`**，先 **`usr_lock`**，再在 **`spin_lock(&disk->lock)`** 内调用 **`sync`**（与 erase 类似，强调与分区删除等临界区配合）。
- **`RT_DEVICE_CTRL_BLK_ERASE`**：若存在 **`ops->erase`**，要求 **`disk->parent.ref_count == 1`**（仅当前持有者），否则 **`-RT_EBUSY`**；成功路径前调用 **`blk_remove_all`** 移除所有已注册分区设备。
- **`RT_DEVICE_CTRL_BLK_AUTOREFRESH`**：可选 **`ops->autorefresh(disk, !!args)`**。
- **`RT_DEVICE_CTRL_BLK_PARTITION`**：对整盘直接 **`-RT_EINVAL`**（分区信息应从 **`rt_blk_device`** 上 `control`）。
- **`RT_DEVICE_CTRL_BLK_SSIZEGET` / `ALL_BLK_SSIZEGET`**：分别调用 **`device_get_blk_ssize`**、**`device_get_all_blk_ssize`**（`blk_dfs.c`）。
- **default**：若存在 **`ops->control`**，则 **`disk->ops->control(disk, RT_NULL, cmd, args)`**（整盘侧 **`blk`** 参数为 `NULL`）。

### 3.4 `rt_hw_blk_disk_register(struct rt_blk_disk *disk)`

主要步骤：

1. 校验 **`disk`**、**`ops`**、设备名非空；**`RT_USING_DM`** 时还要求 **`disk->ida`** 已设置。
2. **DM**：**`rt_dm_ida_alloc`** 分配 **`device_id`**。
3. **`rt_sem_init(&disk->usr_lock, ...)`**，**`rt_list_init(&disk->part_nodes)`**，**`rt_spin_lock_init`**。
4. **`disk->__magic = RT_BLK_DISK_MAGIC`**（用于 **`list_blk`** 等遍历设备表时区分“真·blk 磁盘”与其它 Block 类设备）。
5. 绑定 **`RT_Device_Class_Block`** 的 **open/read/write/control**（或 **`blk_ops` / `blk_parallel_ops`**）。
6. 若无 **`ops->write`**，则 **`read_only = RT_TRUE`**，注册 flag 仅 **`RDONLY`**，否则附加 **`WRONLY`**。
7. **DM**：填写 **`master_id` / `device_id`**。
8. **`device_set_blk_fops(&disk->parent)`**（DFS v2 + POSIX 时挂 **`blk_fops`**）。
9. **`rt_device_register`**；随后 **`rt_blk_disk_probe_partition(disk)`**（分区扫描失败被忽略，不影响注册返回值）。

### 3.5 `rt_hw_blk_disk_unregister`

在 **`spin_lock`** 下检查 **`ref_count`**；**`sync`**；**`rt_sem_detach(usr_lock)`**；**`blk_remove_all`**；**DM ida free**；**`rt_device_unregister`**。

### 3.6 辅助 API

- **`rt_blk_disk_get_capacity`** / **`rt_blk_disk_get_logical_block_size`**：内部 **`getgeome`** 后返回扇区总数或 **`bytes_per_sector`**。

### 3.7 `RT_USING_DFS_MNTTABLE`：`blk_dfs_mnt_table`

**`INIT_ENV_EXPORT`**：关中断遍历所有 **`RT_Object_Class_Device`**，筛选 **`RT_Device_Class_Block`** 且 **`__magic == RT_BLK_DISK_MAGIC`** 的磁盘；若 **`max_partitions == RT_BLK_PARTITION_NONE`** 则对**整盘** **`dfs_mount_device`**；否则对 **`part_nodes`** 上每个分区设备 **`dfs_mount_device`**。用于环境就绪阶段按挂载表自动挂文件系统。

### 3.8 MSH：`list_blk`

在 **`RT_USING_CONSOLE` && `RT_USING_MSH`** 下导出 **`list_blk`**：打印盘符、MAJ:MIN（DM 下）、可移动、容量人类可读、只读、类型（disk/part）、挂载路径（依赖 DFS 查询接口）。子分区以树形前缀 **`|--` / `\`--`** 缩进。

---

## 4. 分区设备层：`blk_dev.c` + `blk_dev.h`

### 4.1 `blk_dev_open` / `close`

直接 **`rt_device_open/close`** 父 **`disk->parent`**，使分区打开时父盘引用计数与电源/时钟策略一致。

### 4.2 `blk_dev_read` / `write`

将分区内的 **`sector`** 加上 **`blk->sector_start`** 后转发 **`rt_device_read/write`** 到父盘。

源码中的区间判断为 **`sector <= blk->sector_start + blk->sector_count`** 且 **`sector_count <= blk->sector_count`**。其中 **`sector` 为分区内的逻辑 LBA**（从 0 起）时，更严格的越界条件应为 **`sector + sector_count <= blk->sector_count`**。阅读或二次开发时建议结合上层调用约定核对边界；若需严格防越界，可在本层加强校验。

### 4.3 `blk_dev_control`

- **GETGEOME**：从父盘取几何，再把 **`sector_count`** 换为 **`blk->sector_count`**，**`bytes_per_sector` / `block_size`** 与整盘一致。
- **SYNC**：透传父盘。
- **ERASE / AUTOREFRESH**：若 **`disk->partitions <= 1`** 才透传，否则 **`-RT_EIO`**（避免多分区下误整盘擦除等）。
- **PARTITION**：**`rt_memcpy(args, &blk->partition, sizeof(...))`**。
- **SSIZEGET / ALL_BLK_SSIZEGET**：与整盘相同辅助函数。
- **default**：**`disk->ops->control(disk, blk, cmd, args)`**，此处 **`blk`** 非空，便于驱动区分整盘与分区。

### 4.4 `blk_dev_initialize`

设置 **`RT_Device_Class_Block`** 与各 **ops** 指针，不完成注册。

### 4.5 `disk_add_blk_dev`

1. **DM**：为新分区设备分配 **`device_id`**。
2. **`blk->disk = disk`**，初始化链表节点。
3. 命名规则：若磁盘名**最后一个字符 `< 'a'`**（例如 **`sd0`**），分区名为 **`"%sp%d"`**（**`sd0p1`**）；否则 **`"%s%d"`**（**`sda1`** 风格）。**DM** 用 **`rt_dm_dev_set_name`**，非 DM 用 **`rt_snprintf`** 写入 **`parent.parent.name`**。
4. **`device_set_blk_fops`**，**`rt_device_register`**，**`spin_lock`** 下将 **`blk->list`** 插入 **`disk->part_nodes`**。

### 4.6 `disk_remove_blk_dev`

若编译 **`RT_USING_DFS`** 且分区已挂载，则 **`dfs_unmount`**；**DM ida free**、**`rt_device_unregister`**、从链表摘除、**`--disk->partitions`**。**`lockless`** 为真时由调用方已持有 **`disk->lock`**（例如 **`blk_remove_all`** 在 erase 路径中）。

### 4.7 `blk_request_ioprio`

返回当前线程调度优先级（供驱动做 I/O 优先级占位）。

---

## 5. 分区探测：`blk_partition.c` + `partitions/*`

### 5.1 `partition_list` 与探测顺序

**`blk_partition.c`** 中函数指针数组顺序为：

1. **`efi_partition`**（**`RT_BLK_PARTITION_EFI`**）
2. **`dfs_partition`**（**`RT_BLK_PARTITION_DFS`**）

依次调用；若某次返回 **`0`**（成功且已建分区）则整体成功；**`-RT_ENOMEM`** 则中止并向上返回。

### 5.2 `rt_blk_disk_probe_partition`

- 若 **`disk->partitions` 已有非 0**（已探测过），直接返回。
- **`max_partitions == RT_BLK_PARTITION_NONE`**：认为不支持分区表，返回 **`-RT_EEMPTY`**，且不打子分区设备。
- 否则遍历 **`partition_list`**。
- 若仍失败或 **`disk->partitions == 0`**：调用 **`blk_put_partition(disk, RT_NULL, 0, total_sectors, 0)`**，即**整盘作为一个逻辑分区 `partno=0`** 注册，保证上层总有一个可挂载块设备。

### 5.3 `blk_put_partition`

1. **`rt_calloc`** 分配 **`struct rt_blk_device`**，**`blk_dev_initialize`**。
2. 填写 **`partno`、`sector_start`、`sector_count`**，以及 **`partition.offset/size`**，**`partition.lock = &disk->usr_lock`**（与整盘用户锁共用，便于 DFS 层同步）。
3. **`disk_add_blk_dev`**，成功则 **`++disk->partitions`**。
4. 若 **`type`** 非 **`"dfs"`**（或与 **`"dfs"`** 不同），会 **`rt_kprintf`** 打印分区大小（人类可读单位）。

### 5.4 `dfs_partition`（`partitions/dfs.c`）

- 分配一扇区大小的缓冲，**`read(disk, 0, ...)`** 读 MBR/描述扇区。
- 循环 **`i < disk->max_partitions`**，调用 **`dfs_filesystem_get_partition`** 解析；成功则 **`blk_put_partition(disk, "dfs", offset, size, i)`**；若返回 **`-RT_ENOMEM`** 则中断。

依赖 **`RT_USING_DFS`** 与 **`RT_BLK_PARTITION_DFS`**。

### 5.5 `efi_partition`（`partitions/efi.c`）

体量较大，核心流程：

- **`force_gpt`**：可通过设备树 bootargs **`gpt`** 等（**`RT_USING_OFW`**）强制 GPT 路径。
- **`efi_crc32`**：EFI 规范 CRC32。
- **`find_valid_gpt`**：读保护性 MBR、主/备 GPT 头与分区表项，校验签名、CRC、LBA 范围，必要时在主次 GPT 间择优。
- **`efi_partition`**：枚举 **`gpt->num_partition_entries`**（上限 **`disk->max_partitions`**），对每个有效 PTE **`blk_put_partition(disk, "gpt", start, size, i)`**。

依赖 **`RT_BLK_PARTITION_EFI`**。

**注意**：**`read_lba`** 等路径以 **512 字节步进**读 GPT 相关扇区，若磁盘 **`bytes_per_sector != 512`**，需确认 BSP 的 **`read`** 语义与 GPT 布局是否一致（常见 eMMC/SD 为 512 逻辑扇区）。

---

## 6. DFS / POSIX 块设备 fops：`blk_dfs.c`

### 6.1 `device_get_blk_ssize` / `device_get_all_blk_ssize`

通过 **`RT_DEVICE_CTRL_BLK_GETGEOME`** 取几何，向 **`args`** 拷 **`bytes_per_sector`** 或 **`bytes_per_sector * sector_count`**（**`rt_uint64_t`**）。

### 6.2 `device_set_blk_fops`

当定义 **`RT_USING_POSIX_DEVIO`** 且 **`RT_USING_DFS_V2`** 时，将 **`struct rt_device`** 的 **`fops`** 设为 **`blk_fops`**，提供：

- **open**：分配 **`blk_fops_data`**（缓存 **geometry**），设置 **`vnode->size`**（**`ALL_BLK_SSIZEGET`**）。
- **read/write**：以**扇区对齐**方式用 **`rt_malloc` 单扇区缓冲**做首尾非对齐字节的读改写，中间段直接 **`rt_device_read/write`** 多扇区。
- **ioctl**：转发 **`rt_device_control`**。
- **flush**：**`RT_DEVICE_CTRL_BLK_SYNC`**。
- **poll**：返回空 mask。
- **lseek**：**`generic_dfs_lseek`**。

未满足上述宏时 **`device_set_blk_fops`** 为空函数，块设备仅能通过 **`rt_device_*`** 扇区接口使用。

---

## 7. Kconfig 与构建

- **`RT_USING_BLK`**：总开关；打开后 **rsource** `partitions/Kconfig`。
- **分区类型**：
  - **`RT_BLK_PARTITION_DFS`**：依赖 **`RT_USING_DFS`**，默认 y。
  - **`RT_BLK_PARTITION_EFI`**：GPT，默认 y。

**`block/SConscript`**：固定编译 **`blk.c`、`blk_dev.c`、`blk_dfs.c`、`blk_partition.c`**，并递归子目录中带 **`SConscript`** 的 **`partitions`**，**`CPPPATH`** 指向 **`components/drivers/include`**。

---

## 8. BSP 接入要点小结

1. 填充 **`struct rt_blk_disk`**：**`parent`** 名称、**`ops`**、**`max_partitions`**（最大分区数，或 **`RT_BLK_PARTITION_NONE`** 关闭分区）、**`RT_USING_DM`** 时设置 **`ida`**。
2. 按需设置 **`read_only`、`parallel_io`、`removable`**。
3. 实现 **`rt_blk_disk_ops`**：**`read`/`getgeome`** 必填；**`write`** 可选；**`sync`/`erase`/`autorefresh`/`control`** 按硬件能力选填。
4. 调用 **`rt_hw_blk_disk_register(disk)`**；卸载路径对称调用 **`rt_hw_blk_disk_unregister`**。

---

## 9. 小结

| 维度 | 内容 |
|------|------|
| 职责 | 块设备**类框架**：整盘设备、分区子设备、GPT/DFS 探测、DFS 自动挂载表、MSH 列表 |
| 并发 | 默认 **`usr_lock`** 串行化整盘读写；**`parallel_io`** 可关闭该语义 |
| 分区回退 | 无有效表时 **`blk_put_partition(..., 0, total_sectors, 0)`** 暴露整盘 |
| 与 DFS 关系 | 分区结构 **`dfs_partition`**、卸载 **`dfs_unmount`**、可选 **`blk_fops`** 字节访问 |

按上述文件顺序阅读：**`blk.h`**（数据模型）→ **`blk.c`**（整盘生命周期与 I/O）→ **`blk_dev.c`**（分区映射）→ **`blk_partition.c` + partitions**（表格式）→ **`blk_dfs.c`**（VFS 侧接口），即可与具体 **`drv_sd`**、MMC、NVMe 等驱动对照串联。
