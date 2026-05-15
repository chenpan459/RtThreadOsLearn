# RT-Thread 5.2.0 SCSI 子系统代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/scsi` 目录实现的 **小型 SCSI 协议栈与块设备绑定**：在 **`RT_USING_DM`** 前提下，由 **`struct rt_scsi_host`** 提供 **`reset`/`transfer`** 与 **HBA 设备指针**；**`rt_scsi_host_register`** 按 **Target ID × LUN** 扫描总线，**INQUIRY** 得到 **`rt_scsi_device`** 后按 **Peripheral Device Type** 分派 **`scsi_sd_probe`** 或 **`scsi_cdrom_probe`**，将磁盘挂到 **通用块设备（`rt_blk_disk`）**。命令 CDB 与数据结构在 **`components/drivers/include/drivers/scsi.h`**；**`rtdevice.h`** 在 **`RT_USING_SCSI`** 下包含 **`drivers/scsi.h`**。

---

## 1. 目录与编译

| 文件 | 条件 | 作用 |
|------|------|------|
| **`scsi.c`** | **`RT_USING_SCSI`** | **Host 注册/扫描**、**`rt_scsi_*` 命令封装**、**按类型分派 probe** |
| **`scsi_sd.c`** | **`RT_SCSI_SD`**（依赖 **`RT_USING_BLK`**） | **DIRECT ACCESS（磁盘）→ `sdX` 块设备** |
| **`scsi_cdrom.c`** | **`RT_SCSI_CDROM`** | **CD-ROM（只读）→ `cdrom%u` 块设备** |
| **`Kconfig`** | — | **`RT_USING_SCSI`**、**`SOC_DM_SCSI_DIR`** 扩展 |
| **`SConscript`** | — | 按选项追加 **`scsi_sd.c`/`scsi_cdrom.c`** |

设计说明（源码注释）：**不把 SCSI 做成系统级 DM 总线**，而是由 **具体 HBA 驱动** 持有 **`rt_scsi_host`** 并调用 **`rt_scsi_host_register`** 完成 **逻辑扫描**。

---

## 2. Kconfig

| 选项 | 含义 |
|------|------|
| **`RT_USING_SCSI`** | 总开关；**`depends on RT_USING_DM`**（默认 **n**） |
| **`RT_SCSI_SD`** | **磁盘** 类设备绑定块层（默认 **y**） |
| **`RT_SCSI_CDROM`** | **光驱** 类设备（默认 **y**） |

---

## 3. `scsi.h`：CDB、命令联合体与对象

### 3.1 打包结构

使用 **`rt_packed`** 与 **`rt_be16`/`rt_be32`/`rt_be64`** 描述 **大端 CDB** 与 **标准响应**（**Inquiry、Request Sense、Read Capacity 10/16、Read/Write 10/12/16、Sync Cache、Write Same、Mode Select/Sense** 等）。

### 3.2 `struct rt_scsi_cmd`

- **`op`**：**各命令 CDB 联合体** + **`op_size`**（下发长度）。
- **`data`**：小响应内嵌 **inquiry/capacity/sense**；大数据路径 **`ptr` + `size`**（**`read`/`write` 走 `ptr`**）。

### 3.3 `struct rt_scsi_host` / `struct rt_scsi_device` / `struct rt_scsi_ops`

- **Host**：**`dev`**、**`ops`**、**`max_id`/`max_lun`**、**`lun_nodes`**（挂 **已发现 `rt_scsi_device`**）。
- **Device**：**`host`**、**`id`/`lun`**、**`devtype`/`removable`**、**`last_block`/`block_size`**（**capacity 成功后填充**）、**`priv`**（给 **sd/cdrom** 私有）。
- **Ops**：**`reset(sdev)`**、**`transfer(sdev, cmd)`**（**由 HBA 完成 DMA/同步/超时**）。

### 3.4 内联 **`rt_scsi_cmd_is_write`**

实现里 **`write10`/`write12`/`write16` 的 opcode 与宏比较存在错位**（例如 **`write16.opcode` 与 `RT_SCSI_CMD_WRITE12`** 比较），**不宜作为可靠 API 使用**；判断写命令应以 **opcode 数值** 或 **调用点上下文** 为准。

---

## 4. `scsi.c`：Host 扫描与命令层

### 4.1 静态 **`scsi_driver driver_table[]`**

以 **`SCSI_DEVICE_TYPE_DIRECT`** 对应 **`scsi_sd_*`**，**`SCSI_DEVICE_TYPE_CDROM`** 对应 **`scsi_cdrom_*`**；其余类型 **无 probe** → **`LOG_E` + `-RT_ENOSYS`**。

### 4.2 **`scsi_device_setup`**

1. 可选 **`host->ops->reset`**。
2. 在 **`driver_table[devtype].probe`** 存在前提下，**最多约 5s** 轮询 **`rt_scsi_test_unit_ready`**。
3. 调用 **对应 `probe`**。

### 4.3 **`rt_scsi_host_register`**

- 校验 **`scsi`/`dev`/`ops`**，**`max_id` 与 `max_lun` 非 0**。
- **双层循环**扫描 **(id, lun)**：
  - 临时 **`tmp_sdev`** 调 **`rt_scsi_inquiry`**；失败 **continue**。
  - 若 **`devtype >= SCSI_DEVICE_TYPE_MAX`**：将 **`scsi->max_id/max_lun` 收缩为当前 id/lun** 并 **break**（注释：**简化 SCSI，不处理乱序设备**）。
  - **`rt_malloc` `rt_scsi_device`**，**`scsi_device_setup`** 成功则 **`rt_list_insert_before(&lun_nodes, &sdev->list)`**。
- 若 **链表仍空** 返回 **`-RT_EEMPTY`**。

### 4.4 **`rt_scsi_host_unregister`**

遍历 **`lun_nodes`**：**`list_remove`** → 可选 **`reset`** → **`driver_table[devtype].remove`**。

**注意**：当前实现为 **`if (!driver_table[sdev->devtype].remove) { ...remove(sdev); }`**，条件与语义 **相反**（**`remove` 非空时不调用**）。维护时建议对照 **预期是否应调用 `scsi_sd_remove`/`scsi_cdrom_remove`** 做修正或验证。

### 4.5 **`scsi_transfer` 与 GNU 扩展**

**`transfer` 失败**后常用 **`?: rt_scsi_request_sense`** 尝试 **取 sense**（**GNU 语句表达式**风格）。

### 4.6 典型命令封装

- **`rt_scsi_request_sense`**：**alloc_length 0x12**，数据进 **`request_sense`** 结构。
- **`rt_scsi_test_unit_ready`**：**TUR**；失败再 **sense**。
- **`rt_scsi_inquiry`**：**0x24 字节**；填充 **`sdev->devtype`/`removable`**（**`RMB` 位**）。
- **`rt_scsi_read_capacity10/16`**：成功则 **`last_block`/`block_size` 转 CPU 序写入 `sdev`**。
- **`rt_scsi_read/write 10/12/16`**：**`size` 为块数**，**`data.size = size * block_size`**。
- **`rt_scsi_write_same10/16`**：**`config` 带 `RT_SCSI_UNMAP_SHIFT` 位**（**unmap/精简置零语义由目标解释**）。
- **`rt_scsi_mode_select6/10`**：在 **`buffer` 前拼 4/8 字节头** 再 **`rt_malloc` 下发**。
- **`rt_scsi_mode_sense6/10`**：下发后 **`scsi_mode_sense_fill`** 解析长度/介质类型等；**`mode_sense10` 路径里 `scsi_mode_sense_fill(..., use10)` 传入固定 `RT_FALSE`**，与 **10 字节模式头** 可能不一致，**解析复杂 MODE PAGE 时需在目标板上验证**。

---

## 5. `scsi_sd.c`：SCSI 磁盘 → 块设备

### 5.1 对象 **`struct scsi_sd`**

**`struct rt_blk_disk parent`** + **`struct rt_scsi_device *sdev`** + **`sd_id`**、**`use16`**（**capacity10 返回 0xffffffff 则读 capacity16 并走 16 字节命令**）、**`geometry`**。

### 5.2 **`rt_blk_disk_ops`**

- **`read`/`write`**：**`sector_count` 截断到 32 位**；**`sector >> 32`** 用 **READ/WRITE16**，否则 **10**。
- **`sync`**：**SYNCHRONIZE_CACHE 10 或 16**（**全盘 `lba_count`**）。
- **`erase`**：**WRITE_SAME 10/16**（**带 UNMAP 位**，作 **discard/unmap 类语义**）。
- **`autorefresh`**：**MODE SENSE 6/10** 读 **Caching 页**，改 **bit** 后 **MODE SELECT**（**与具体磁盘 MODE PAGE 实现强相关**）。

### 5.3 **`scsi_sd_probe`**

**`rt_dm_ida_alloc`** 分配 **盘符序号** → **`rt_scsi_read_capacity10`**，若 **`last_block == 0xffffffff`** 再 **`read_capacity16` 且 `use16 = RT_TRUE`** → 填 **geometry** → **`rt_dm_dev_set_name(..., "sd%c%c", letter_name(sd_id))`** → **`rt_hw_blk_disk_register`**。

### 5.4 **`scsi_sd_remove`**

**`rt_dm_ida_free`** + **`rt_hw_blk_disk_unregister`**；**`ssd` 内存** 是否由 **块层 unregister** 释放取决于 **`rt_hw_blk_disk_unregister`** 实现，**与 `probe` 中 `rt_calloc` 配对关系需在块框架中确认**。

---

## 6. `scsi_cdrom.c`：只读光驱

- **`rt_blk_disk_ops`**：**仅 `read` + `getgeome`**（**无写/sync/erase**）。
- **`parent.read_only = RT_TRUE`**，**`max_partitions = RT_BLK_PARTITION_NONE`**。
- **读路径**：**高 LBA 用 READ16，否则 READ12**（**CD 常用 12 字节读**）。
- **设备名**：**`cdrom%u`**。

---

## 7. HBA 驱动集成要点

1. 填充 **`struct rt_scsi_ops`**：**`transfer`** 必填；**`reset`** 可选。
2. 设置 **`max_id`/`max_lun`**（**至少为 1**）。
3. **`scsi->dev`** 指向 **本 HBA 的 `rt_device`**。
4. 在 **硬件与缓冲就绪** 后调用 **`rt_scsi_host_register`**；**卸载** 时 **`rt_scsi_host_unregister`**。

**`transfer` 职责**：根据 **`cmd->op_*` 与 `op_size`** 组 **CDB**，按 **`cmd->data.ptr/size`** 做 **读/写 DMA 或 PIO**，把 **响应填回 `cmd->data` 小结构或缓冲区**，返回 **`RT_EOK` 或负错误码**。

---

## 8. 小结

| 层次 | 职责 |
|------|------|
| **`scsi.h`** | **SCSI 常量、CDB/响应布局、`rt_scsi_host/device/ops`** |
| **`scsi.c`** | **总线扫描、类型分派、常用 CDB 封装** |
| **`scsi_sd.c`** | **磁盘块设备 + 大容量/缓存/unmap** |
| **`scsi_cdrom.c`** | **只读光盘块设备** |

该实现面向 **存储类简化 SCSI**，**非完整 SAM/枚举状态机**；**磁带、处理器设备等** 需在 **`SOC_DM_SCSI_DIR`** 或本目录 **扩展 `driver_table`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/scsi/`。*
