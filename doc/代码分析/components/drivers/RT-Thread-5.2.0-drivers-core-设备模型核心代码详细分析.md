# RT-Thread 5.2.0 drivers/core 设备模型核心代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/core` 目录：在 **`RT_USING_DEVICE`** 之上提供 **经典字符/块设备 API**、可选 **总线–驱动–设备（BDD）模型**、**设备树（DM/OFW）对接**、**根文件系统挂载**、**NUMA 辅助** 与 **电源域（power domain）** 等基础设施。

涉及文件与头文件（部分 API 声明在 `drivers/core/dm.h`、`drivers/core/bus.h`、`drivers/platform.h` 等）：

| 源文件 | 典型编译条件 |
|--------|----------------|
| `device.c` | `RT_USING_DEVICE`（必选进入本组） |
| `bus.c` | `RT_USING_DEV_BUS` **或** `RT_USING_DM` |
| `dm.c`、`driver.c`、`platform.c`、`numa.c`、`power_domain.c` | `RT_USING_DM` |
| `mnt.c` | `RT_USING_DM` 且 `RT_USING_DFS` |
| `platform_ofw.c` | `RT_USING_OFW` |

**`core/Kconfig`**：仅含 **`RT_USING_DM`** 与 **`RT_USING_DEV_BUS`**；更上层 DM 选项多在根 `Kconfig` 与其它子系统。

---

## 1. 构建关系（`SConscript`）

- 始终编译 **`device.c`**，`CPPPATH` 指向 **`components/drivers/include`**。
- **`bus.c`**：在 **`RT_USING_DEV_BUS` 或 `RT_USING_DM`** 时加入（同文件内用 `#ifdef` 分段）。
- **DM 组**：`RT_USING_DM` 时加入 **`dm.c`、`driver.c`、`numa.c`、`platform.c`、`power_domain.c`**；若 **`RT_USING_DFS`** 再增加 **`mnt.c`**。
- **`platform_ofw.c`**：依赖 **`RT_USING_OFW`**。

整组 **`depend = ['RT_USING_DEVICE']`**。

---

## 2. `device.c`：通用设备对象与访问接口

不依赖 DM，属于内核侧 **`struct rt_device`** 生命周期与 I/O 封装。

### 2.1 主要 API

- **`rt_device_register/unregister`**：对象类 **`RT_Object_Class_Device`**，初始化 **`flag`、`ref_count`、`open_flag`**；可选 **POSIX wait_queue**、**DFS devfs** 挂钩。
- **`rt_device_find`**：按名在对象容器查找。
- **`rt_device_create/destroy`**（`RT_USING_HEAP`）：分配带 **`attach_size`** 的设备内存。
- **`rt_device_init`**：若未 **`ACTIVATED`** 且存在 **`init`**，调用一次并置位 **`RT_DEVICE_FLAG_ACTIVATED`**。
- **`rt_device_open/close`**：懒初始化；**`STANDALONE`** 设备防重复打开；**`open_flag`** 与 **`oflag`** 掩码变化时重新 **`device_open`**；**`ref_count`** 在成功路径递增。
- **`rt_device_read/write/control`**：要求 **`ref_count > 0`**，否则 read/write 返回 0 并 **`rt_set_errno(-RT_ERROR)`**。
- **`rt_device_set_rx_indicate` / `set_tx_complete`**。

### 2.2 `RT_USING_DEVICE_OPS`

通过宏将 **`dev->ops->*`** 与 **`dev->init/open/...`** 两套访问方式统一。

---

## 3. `bus.c`：两段式实现

### 3.1 `RT_USING_DEV_BUS`（与 DM 无关的轻量总线设备）

- **`rt_device_bus_create`**：创建 **`RT_Device_Class_Bus`** 设备并注册；**POSIX** 下挂空 **`bus_fops`**。
- **`rt_device_bus_destroy`**：注销并 **`rt_device_destroy`**。

### 3.2 `RT_USING_DM`（总线核心）

全局 **`bus_nodes`** 链表 + **`bus_lock`**，描述 **`struct rt_bus`**：

- **`dev_list` / `drv_list`**：挂接设备与驱动，各带 **`rt_spinlock`**。
- **`match` / `probe` / `remove` / `shutdown`**：由具体总线类型填充（如 platform）。

**`rt_bus_register`**：初始化链表与自旋锁，将 **`bus->list`** 插入 **`bus_nodes`**。

**`rt_bus_add_driver`**：驱动入 **`drv_list`**，**`rt_bus_for_each_dev(..., bus_probe_driver)`** — 对每个设备尝试 **`bus_probe`**。

**`rt_bus_add_device`**：设备入 **`dev_list`**，**`rt_bus_for_each_drv(..., bus_probe_device)`**。

**`bus_probe` 逻辑**：若设备尚无 **`dev->drv`** 且 **`bus->match(drv, dev)`** 为真，则 **`dev->drv = drv`**，调用 **`bus->probe(dev)`**；失败则清除 **`dev->drv`**。匹配成功时 **`drv->ref_count++`**（在 **`bus_probe_device`** 路径）。

**`rt_bus_remove_driver`**：若 **`ref_count != 0`** 则 **`-RT_EBUSY`**。

**`rt_bus_remove_device`**：从链表摘除；优先 **`bus->remove(dev)`**，否则若存在驱动则 **`drv->remove`** 且 **`--drv->ref_count`**。

**`rt_bus_shutdown`**：遍历所有总线，对每个设备调用 **`bus->shutdown`** 或 **`drv->shutdown`**（记录最后一次错误但仍尽量遍历）。

**`rt_bus_find_by_name` / `rt_bus_reload_driver_device`**：查找与将设备迁到另一总线并重新 **`rt_bus_add_device`**。

---

## 4. `driver.c`：驱动注册薄封装

- **`rt_driver_register(drv)`**：若 **`drv->bus`** 非空则 **`rt_bus_add_driver(bus, drv)`**；否则 **`-RT_EINVAL`**。
- **`rt_driver_unregister`**：**`rt_bus_remove_driver`**。

驱动侧需在静态 **`struct rt_driver`** 中填写 **`bus` 指针**（通常指向 **`platform_bus`** 等）。

---

## 5. `dm.c`：设备模型辅助（IDA、命名、OFW 封装、SMP）

### 5.1 IDA（`struct rt_dm_ida`）

- **`rt_dm_ida_alloc/free/take`**：位图 **`RT_DM_IDA_NUM`（256）** 分配 **`device_id`** 等；带 **`ida->lock`**。

### 5.2 设备查找与命名

- **`rt_dm_device_find(master_id, device_id)`**：在设备对象表中按 **`master_id`/`device_id`** 查找（**`master_id <= 0` 直接失败** 为源码条件，调用方需注意约定）。
- **`rt_dm_dev_set_name` / `get_name`**：**`vsnprintf`** 写 **`dev->parent.name`**。
- **`rt_dm_dev_set_name_auto`**：按 **`prefix`** 维护全局递增 uid，生成 **`prefix%u`** 风格名称。
- **`rt_dm_dev_get_name_id`**：从设备名尾部解析数字 id。

### 5.3 OFW 相关封装（`#ifdef RT_USING_OFW`）

对 **`dev->ofw_node`** 统一转 **`rt_ofw_*`**：

- 地址：**`get_address_count/get_address/get_address_by_name/get_address_array`**。
- MMIO：**`iomap` / `iomap_by_name`**。
- 中断（需 **`RT_USING_PIC`**）：**`get_irq_count/get_irq/get_irq_by_name`**。
- 属性：**各类 `prop_read_*`、`prop_count_of_size`、`prop_index_of_string`、`prop_read_bool`**。
- **`rt_dm_dev_bind_fwdata` / `unbind_fwdata`**：绑定 **`ofw_node`** 与 **`rt_ofw_data`**。

**注意**：**`rt_dm_dev_prop_read_u8_array_index`** 中条件编译宏写为 **`RT_UISNG_OFW`**（拼写错误），且参数名为 **`out_value`** 与声明不一致，**该 u8 数组接口在默认宏下实际未编译进 OFW 分支**；若需使用请核对上游是否已修复。

### 5.4 SMP（`RT_USING_SMP`）

**`rt_dm_secondary_cpu_init`**：在次级 CPU 上执行 **`INIT_EXPORT` 段 `6.end`～`7.end`** 之间的初始化函数（与主核自动初始化表分离）。

---

## 6. `platform.c`：Platform 总线与设备/驱动注册

### 6.1 静态 **`platform_bus`**

- **`name = "platform"`**。
- **`match`**：有 **`ofw_node`** 时 **`rt_ofw_node_match(np, pdrv->ids)`**；否则 **`pdev->name` 与 `pdrv->name` 字符串比较**（支持指针相等短路）。
- **`probe`**：**`rt_dm_power_domain_attach(dev, RT_TRUE)`**；成功则调 **`pdrv->probe(pdev)`**；成功且存在 DT 节点则 **`rt_ofw_node_set_flag(np, RT_OFW_F_READLY)`**；失败时 **`ENOMEM` 打日志** 并 **`rt_dm_power_domain_detach`**。
- **`remove` / `shutdown`**：调驱动对应钩子，**`rt_dm_power_domain_detach`**，**`rt_platform_ofw_free`**。

### 6.2 对外接口

- **`rt_platform_device_alloc`**：**`rt_calloc`**，预设 **`parent.bus = &platform_bus`**。
- **`rt_platform_driver_register`**：设置 **`pdrv->parent.bus`** 与驱动名，**`rt_driver_register(&pdrv->parent)`**。
- **`rt_platform_device_register`**：**`rt_bus_add_device(&platform_bus, &pdev->parent)`**。

### 6.3 初始化

**`INIT_CORE_EXPORT(platform_bus_init)`**：**`rt_bus_register(&platform_bus)`**。

---

## 7. `platform_ofw.c`：设备树枚举与延迟请求

### 7.1 作为“总线桥”的 compatible

**`platform_ofw_ids`**：`simple-bus`、可选 **`simple-mfd`、`isa`、`arm,amba-bus`** 等。匹配到且 **有子节点** 时 **递归 `platform_ofw_device_probe_once`**，实现自根向下展开。

### 7.2 **`platform_ofw_device_probe_once`**

对每个 **可用子节点**：跳过已有 **`np->dev`**、系统/已 probe 标记、无 **name/compatible** 的占位节点；对桥 compatible 先递归子树；再 **`alloc_ofw_platform_device`**（**`rt_platform_device_alloc("")`**，**`ofw_node_get`**，**`ofw_device_rename`**，设 **`ofw_node`**），**`rt_platform_device_register`**。

### 7.3 **`ofw_device_rename`**

沿父链查找带 **reg** 的节点，用 **地址 + 节点名（含 `@` 截断）+ mask** 等生成 **`rt_dm_dev_set_name`** 字符串，避免重名。

### 7.4 **`rt_platform_ofw_request(np)`**

供 **时钟等模块** 在解析 phandle 时 **按需创建 platform 设备**：若节点已有 **`dev`** 且已 **`drv`** 则成功；若 **`dev` 无 `drv`** 则 **`rt_bus_reload_driver_device`**；若无 **`dev`** 则分配并 **`rt_platform_device_register`**。

### 7.5 启动入口

**`INIT_PLATFORM_EXPORT(platform_ofw_device_probe)`**：从 **`ofw_node_root`** 调用 **`platform_ofw_device_probe_once`**，并额外扫描 **`/firmware`、`/clocks`、chosen 下 `simple-framebuffer`** 等子树。

### 7.6 **`rt_platform_ofw_free`**

清除 **`RT_OFW_F_PLATFORM`**，**`rt_ofw_node_put`**，释放 **`pdev`**。

---

## 8. `mnt.c`：根文件系统与环境挂载

依赖 **`RT_USING_OFW`**（`#else` 直接 **`#error`**）。

- **`INIT_ENV_EXPORT(rootfs_mnt_init)`**：解析 **`root=`、`rootfstype=`、`rw`、`rootwait`、`rootdelay=`** 等 bootargs；若无块设备根，可尝试 **memblock 中 `initrd` 保留区** **`rt_ioremap_cached`** 后按 **`crom`** 类型挂载；对 **`rootwait`** 轮询 **`rt_device_find`**（注释说明为兼容 SDIO 异步初始化带来的等待）；最终 **`dfs_mount`**。
- **`INIT_FS_EXPORT(fstab_mnt_init)`**：**`mkdir("/mnt")`**，在 **`RT_USING_FINSH`** 下 **`msh_exec_script("fstab.sh", 16)`**。

---

## 9. `numa.c`：NUMA 轻量支持

- **`numa_ofw_init`**（**`INIT_CORE_EXPORT`**）：若 bootargs **`numa=on`**，置 **`numa_enabled`**，遍历 CPU 节点填 **`cpu_numa_map`**，遍历 **`memory` 类型节点** 记录 **`numa_memory`** 区间链表。
- **`rt_numa_cpu_id` / `rt_numa_device_id`**：查询 CPU / 设备 **`numa-node-id`**。
- **`rt_numa_memory_affinity`**：根据物理地址生成 **IRQ affinity 位图**（落在某 NUMA 的内存则绑定到对应节点的 CPU）。

未开启时 **`rt_numa_memory_affinity`** 默认亲和 CPU0。

---

## 10. `power_domain.c`：电源域树与设备 attach

- **`rt_dm_power_domain_register*`**：初始化 **`rt_dm_power_domain`**（链表、**`rt_ref`**、自旋锁等）。
- **`rt_dm_power_domain_power_on/off`**：带 **子域递归** 与失败回滚；**`ref`** 与 **`power_on/power_off` 回调** 配合；**`dm_power_domain_release`** 仍为 **`RT_ASSERT(0)`** 占位。

**OFW**：**`ofw_find_power_domain`** 解析 **`power-domains`** phandle，区分 **`RT_POWER_DOMAIN_OBJ_NAME`** 与 **`RT_POWER_DOMAIN_PROXY_OBJ_NAME`**（proxy 的 **`ofw_parse`** 返回真实 **`rt_dm_power_domain*`**）。

- **`rt_dm_power_domain_attach(dev, on)`**：默认取 **索引 0** 的 power domain，分配 **`power_domain_unit`**，**`attach_dev`** 回调后挂 **`unit_nodes`**；若 **`on`** 则 **`power_on`**。
- **`rt_dm_power_domain_detach(dev, off)`**：**`detach_dev`**、摘链、**`power_off`**（若 **`off`**）。

---

## 11. 模块协作关系（简图）

```text
INIT_CORE: platform_bus_init → bus 注册
INIT_PLATFORM: platform_ofw_device_probe → 创建设备 → rt_bus_add_device
                → match(platform) → probe → power_domain_attach → pdrv->probe

应用/驱动: rt_device_*（device.c）
          rt_dm_dev_* / rt_clk_get_*（dm.c + 其它子系统）
关机: rt_bus_shutdown（bus.c）
```

---

## 12. 小结

| 维度 | 内容 |
|------|------|
| **`device.c`** | 与 DM 正交的 **设备 I/O 与引用计数语义** |
| **`bus.c` + `driver.c` + `platform.c`** | **总线匹配 + probe/remove** 的 DM 骨架 |
| **`platform_ofw.c`** | **FDT 展开为 `rt_platform_device`** 及 **按需 `rt_platform_ofw_request`** |
| **`dm.c`** | **IDA、命名、OFW 属性/中断/IO 映射封装**（注意 u8 属性宏拼写问题） |
| **`mnt.c` / `numa.c` / `power_domain.c`** | 启动期 **根 FS**、可选 **NUMA**、**power-domains** 与 **probe 顺序** 耦合 |

阅读顺序建议：**`drivers/core/bus.h` + `drivers/core/driver.h`（若需）** → **`device.c`** → **`bus.c`（DM 段）** → **`platform.c`** → **`platform_ofw.c`** → **`dm.c`** → **`power_domain.c`**，再按需看 **`mnt.c`/`numa.c`**。
