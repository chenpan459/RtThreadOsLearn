# RT-Thread 5.2.0 OFW 设备树（Open Firmware）子系统代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/ofw` 目录：在 **`RT_USING_DM`** 前提下，将 **Flattened Device Tree（FDT）** 解析为 **`struct rt_ofw_node` 树**，提供与 **Linux Devicetree** 相近的 **属性读写、phandle、地址翻译、`ranges`/`dma-ranges`、中断映射、earlycon** 等能力；底层打包 **libfdt**（**`libfdt/*.c`**）。对外头文件由 **`rtdevice.h`** 在 **`RT_USING_OFW`** 下包含：**`ofw.h`、`ofw_fdt.h`、`ofw_io.h`、`ofw_irq.h`、`ofw_raw.h`**。

---

## 1. 依赖与 Kconfig

| 选项 | 含义 |
|------|------|
| **`RT_USING_OFW`** | 总开关；**`depends on RT_USING_DM`**；**`select`** **ADT（ref/bitmap）**、**`RT_USING_MEMBLOCK`** |
| **`RT_USING_BUILTIN_FDT`** | 内核内置 **DTB**；**`RT_BUILTIN_FDT_PATH`** 默认 **`rtthread.dtb`** |
| **`RT_FDT_EARLYCON_MSG_SIZE`** | **earlycon** 环形缓冲大小（**KB**），默认 **128** |
| **`RT_USING_OFW_BUS_RANGES_NUMBER`** | **`io.c`** 中 **`bus_ranges`** 槽位数（32/64 位默认 **4/8**） |

**`SConscript`**：

- 无 **`RT_USING_OFW`** 则整个 **`ofw`** 不参与编译。
- **`ofw/*.c`** 加入 **`DeviceDrivers`**；子目录 **`libfdt/`** 另有 **`SConscript`** 编 **libfdt** 各 **`.c`**。
- **`GetDepend('RT_USING_PIC') == False`** 时 **`SrcRemove(src, ['irq.c'])`**：无 **PIC** 时不编 **OFW 中断解析**（与 **`drivers/pic`** 解耦）。

---

## 2. 目录与文件职责

| 路径 | 职责 |
|------|------|
| **`base.c`** | **`rt_ofw_node`/`rt_ofw_prop` 树**构建与销毁、**phandle** 哈希、**别名（aliases）**、节点遍历/查找、**`rt_ofw_parse_phandle(_cells)`**、属性 **u8/u16/u32/u64/string** 系列读取、**`rt_ofw_map_id`**、**`rt_ofw_node_get/put`（引用计数）** 等主体逻辑（体量最大） |
| **`fdt.c`** | **FDT blob** 管理：**`rt_fdt_prefetch`/`scan_root`/`scan_memory`/`scan_initrd`**、**`rt_fdt_unflatten`** 生成 **`rt_ofw_node`**、**`rt_fdt_read_number`/`translate_address`**、**earlycon**（**`RT_FDT_EARLYCON_EXPORT`**）、**`rt_fdt_commit_memregion_*`** 与 **memblock** 协作 |
| **`io.c`** | **`#address-cells`/`#size-cells`** 沿父链解析、**`reg`/`reg-names`** 取址、**`ranges`/`dma-ranges`** 地址正反向翻译、**`rt_ofw_iomap`** |
| **`irq.c`**（**`RT_USING_PIC`**） | **`#interrupt-cells`**、**`interrupt-map`/`interrupt-map-mask`****、**`interrupt-parent`**、**`rt_ofw_map_irq`** 与 **PIC** 绑定、**`rt_ofw_get_irq*`** |
| **`ofw.c`** | **`rt_ofw_stub_probe_range`**（按 **`.rt_ofw_data.*` 段** 扫描 **`rt_ofw_stub`**）、**`rt_ofw_parse_object`**（从 **`rt_ofw_data(np)`** 解析 **clock/reset/power-domain** 等复合 **`rt_object`**）、**`rt_ofw_console_setup`**、**`rt_ofw_bootargs_select`** 等 glue |
| **`raw.c`** | 对 **libfdt** 的薄封装：**`fdt_add_subnode_possible`**（空间不足则 **`fdt_open_into` 扩容**）、**`fdt_setprop_uxx`**、**`fdt_getprop_u8/16/32/64`** 等 |
| **`ofw_internal.h`** | **`fdt_info`、`alias_info`、`bus_ranges`**、全局根节点指针、**phandle** 内部 API |
| **`libfdt/`** | 上游 **libfdt**：**只读/读写/overlay/sw/wip** 等，供 **`raw.c`/`fdt.c`/`base.c`** 调用 |

---

## 3. 核心数据模型（`ofw.h`）

### 3.1 `struct rt_ofw_prop`

单向链表：**`name`、`length`、`value`** 指向 **FDT 内或拷贝后的** 属性 blob。

### 3.2 `struct rt_ofw_node`

- **树**：**`parent` / `child` / `sibling`**
- **标识**：**`name`、`full_name`、`phandle`**
- **与 DM 绑定**：**`struct rt_device *dev`**
- **私有数据**：**`rt_ofw_data(np)`** 宏 → **`rt_data`**（驱动 **`probe`** 后常指向 **`rt_platform_device` 控制器** 或子系统对象）
- **生命周期**：**`struct rt_ref ref`**，**`rt_ofw_node_get/put`**
- **标志位**：**`RT_OFW_F_SYSTEM`/`READLY`/`PLATFORM`/`OVERLAY`**（**`READLY` 拼写** 与头文件一致）

### 3.3 `struct rt_ofw_cell_args`

解析 **phandle + cells** 的结果：**`data`** 为目标节点指针，**`args[]`** 为附加 cell，**`args_count`** 为个数（上限 **`RT_OFW_MAX_CELL_ARGS`**）。

### 3.4 Stub 机制

**`struct rt_ofw_stub`**：**`ids` + `handler(np, id)`**，通过 **`RT_OFW_STUB_EXPORT`** 放入链接段，**`rt_ofw_stub_probe_range`** 在 **compatible** 命中且节点 **available**、非 **SYSTEM/READLY** 时调用；成功处理可置 **`RT_OFW_F_READLY`**。

---

## 4. FDT 引导与展开（`fdt.c` / `ofw_fdt.h`）

典型启动路径（逻辑顺序，以源码为准）：

1. **`rt_fdt_prefetch`**：定位/校验 **FDT**（含 **builtin** 路径）。
2. **`rt_fdt_scan_*`**：根节点、**`/memory`**、**initrd**、**chosen/stdout** 等。
3. **`rt_fdt_unflatten` / `rt_fdt_unflatten_single`**：生成 **`rt_ofw_node`** 全树，建立 **phandle** 映射、**aliases** 扫描等（与 **`base.c`** 协同）。
4. **`rt_fdt_earlycon_output`/`kick`**：在正式 **console** 前输出日志；**`struct rt_fdt_earlycon`** 内含 **MMIO/端口**、**`console_putc`**、**消息缓冲**。

**`rt_fdt_node_name`**：从 **`full_name`** 取 **`/`** 后段，即 **unit name**。

---

## 5. 地址与 MMIO（`io.c` / `ofw_io.h`）

- **`rt_ofw_bus_addr_cells` / `bus_size_cells`**：自 **`np`** 向父查找 **`#address-cells`/`#size-cells`**，根缺省 **(1,1)**（与 **`ofw_internal.h`** 宏一致）。
- **`rt_ofw_get_address`**：按 **`reg`** 第 **`index`** 组解析 **物理基址+长度**（结合父总线 **cells**）。
- **`rt_ofw_translate_address` / `reverse_address`**：沿 **`ranges` 或 `dma-ranges`**（**`range_type`** 字符串）做 **子总线地址 ↔ CPU 可访问地址** 转换；**`ofw.h`** 内联 **`dma2cpu`/`cpu2dma`** 组合两次 **`reverse` + `translate`**。
- **`rt_ofw_iomap`**：**`rt_ioremap`** 映射 **`reg`** 指定 **bank**。

**`bus_ranges`**：用于缓存/加速多段 **ranges**（槽位上限 **`RT_USING_OFW_BUS_RANGES_NUMBER`**）。

---

## 6. 中断（`irq.c` / `ofw_irq.h`）

（仅 **`RT_USING_PIC`** 编译）

- **`rt_ofw_irq_cells`**：读 **`#interrupt-cells`**
- **`rt_ofw_parse_irq_map`**：大段注释描述 **Devicetree spec** 中 **interrupt-map** 五元组语义，实现子域 → 父 **PIC** 的映射
- **`rt_ofw_find_irq_parent`**：沿 **`interrupt-parent`** 上溯
- **`rt_ofw_map_irq`**：将 **`rt_ofw_cell_args`** 转为 **Linux 风格 `hwirq`**（与 **PIC 驱动**对接）
- **`rt_ofw_get_irq` / `get_irq_by_name`**：从 **`interrupts`/`interrupt-names`** 取 **逻辑 IRQ 号**

---

## 7. 运行时 FDT 修改（`raw.c` / `ofw_raw.h`）

在 **可写 FDT 镜像** 上：

- **`fdt_add_subnode_possible`**：**`fdt_add_subnode`** 失败则 **`fdt_open_into`** 增加 **`FDT_PADDING_SIZE`** 再试。
- **`fdt_add_mem_rsv_possible`**：同理扩展后 **`fdt_add_mem_rsv`**。
- **`fdt_setprop_uxx`**：统一 **32/64** **`fdt_setprop_*`**。
- **`fdt_getprop_u8`…`u64`**：类型化读取 **cell**。

---

## 8. 合成对象解析（`ofw.c`）

**`rt_ofw_parse_object(np, obj_name, cells_name)`**：当 **`rt_ofw_data(np)`** 指向一块连续 **`struct rt_object`** 区域（例如 **clock provider** 在 **`probe`** 里一次性 **`rt_calloc`** 多个 **`rt_clk_node`**）时，按 **`ofw_obj_cmp_list`**（**`#clock-cells`**、**`#reset-cells`**、**`#power-domain-cells`** 等）与 **`obj->name`** 步进匹配，返回对应子对象。用于 **一个 OFW 节点对应多个内核对象** 的少见布局。

---

## 9. libfdt 子目录

实现 **FDT v0.17** 常规操作：**`fdt_check_header`、`fdt_getprop`、`fdt_subnode_offset`、`fdt_path_offset`**、**overlay**、**读写 API** 等。许可证与上游 **libfdt** 一致（ SPDX 见各文件头），**一般不需修改**，除非合并上游安全修复。

---

## 10. 与 DM 的关系

- **平台设备**：**`dev->ofw_node`** 指向 **`rt_ofw_node`**；**`rt_ofw_data(np)`** 常存 **`struct rt_platform_device *` 或控制器上下文**。
- **延迟驱动**：**`rt_platform_ofw_request(np)`**（在其它子系统如 **mailbox** 中可见）依赖 **OFW** 对 **phandle 目标** 的 **probe** 顺序控制。
- **控制台**：**`rt_ofw_console_setup`** 根据 **chosen** 安装 **serial** 等。

---

## 11. 阅读建议

1. 先读 **`ofw.h`** 的数据结构与 **宏 API**（**`rt_ofw_prop_read_*`、`foreach_*`**）。
2. 再读 **`ofw_fdt.h` + `fdt.c`** 弄清 **FDT → 内存树** 的时机与 **earlycon**。
3. **`base.c`** 中选 **phandle 哈希、`rt_ofw_parse_phandle_cells`、节点销毁** 等路径跟踪。
4. 做驱动集成时重点用 **`ofw_io.h`/`ofw_irq.h`**；需改 **DTB** 时用 **`raw.c`/`libfdt`**。

---

## 12. 小结

| 层级 | 内容 |
|------|------|
| 规范对齐 | **FDT + Devicetree** 常见属性（**reg、interrupts、ranges、phandle**） |
| 实现拆分 | **fdt（blob 与引导）/ base（树与属性）/ io（地址）/ irq（PIC）/ raw（可写）** |
| 第三方 | **libfdt** 完整子树 |

**`base.c` 代码量极大**，文档无法逐函数展开；以上按 **子文件职责** 划分，便于按需 **`grep`** 深入。
