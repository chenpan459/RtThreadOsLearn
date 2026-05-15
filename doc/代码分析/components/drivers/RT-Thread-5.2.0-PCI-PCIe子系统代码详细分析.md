# RT-Thread 5.2.0 PCI/PCIe 子系统代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/pci` 目录实现的 **PCI/PCIe 核心框架**：**Host Bridge → Root Bus → 枚举与配置空间访问 → 设备绑定 `rt_pci_driver`**，以及可选的 **ECAM**、**MSI/MSI-X**、**Endpoint（RC 对端）**、**DesignWare PCIe IP** 等宿主控制器实现。对外接口集中在 **`components/drivers/include/drivers/pci.h`**（含 **`pci_regs.h`/`pci_ids.h`** 寄存器与 ID 常量），**`rtdevice.h`** 在 **`RT_USING_PCI`** 下包含 **`pci.h`**，**`RT_PCI_MSI`** / **`RT_PCI_ENDPOINT`** 下再包含 **`pci_msi.h`** / **`pci_endpoint.h`**。

---

## 1. 依赖与 Kconfig 总览

| 选项 | 含义 |
|------|------|
| **`RT_USING_PCI`** | 总开关；**`depends on RT_USING_DM`、`RT_USING_PIC`**；**`select`** ADT/bitmap |
| **`RT_PCI_MSI`** | 编译 **`msi/`** 下 **MSI/MSI-X** 支持（默认 **y**） |
| **`RT_PCI_ENDPOINT`** | 编译 **`endpoint/`**；**`select RT_USING_ADT_REF`** |
| **`RT_PCI_SYS_64BIT`** | 64 位 PCI 地址空间语义（**`ARCH_CPU_64BIT`**） |
| **`RT_PCI_CACHE_LINE_SIZE`** | 缓存行（**DWORD** 为单位配置） |
| **`RT_PCI_LOCKLESS`** | **`access.c`** 中配置空间访问**不加全局自旋锁**（**`RT_PCI_LOCKLESS`**） |
| **`RT_PCI_ECAM`** | 编 **`ecam.c`**，提供 **ECAM map/read/write** 模板（默认 **y**） |

**`rsource host/Kconfig`**：**`RT_PCI_HOST_COMMON`**、**`RT_PCI_HOST_GENERIC`**（依赖 **ECAM**）；**`dw/Kconfig`** 为 **Synopsys DesignWare PCIe** 主机/EP 片段。

---

## 2. 顶层 `SConscript` 与源码划分

**`pci/SConscript`**（无 **`RT_USING_PCI`** 直接返回）：

- 核心：**`access.c`、`host-bridge.c`、`irq.c`、`pci.c`、`pme.c`、`probe.c`**
- **`RT_USING_OFW`**：**`ofw.c`**（**Host Bridge 与 DT** 绑定）
- **`RT_PCI_ECAM`**：**`ecam.c`**
- 递归子目录：**`msi/`、`endpoint/`、`host/`**（各自 **`SConscript`**）

---

## 3. 拓扑与核心对象（`pci.h`）

### 3.1 总线资源

- **`struct rt_pci_bus_region`**：**物理/CPU 地址、长度、`bus_start`、flags（MEM/IO/PREFETCH）**
- **`struct rt_pci_bus_resource`**：单段 **BAR 类资源**（**`base/size/flags`**）

### 3.2 `struct rt_pci_host_bridge`

- **`domain`**、**`root_bus`**、**`ops`/`child_ops`**（配置访问与下层总线）
- **`bus_range[2]`**、**`bus_regions`/`dma_regions`**（CPU↔PCI 窗口）
- **`irq_slot`/`irq_map`**：平台把 **INTx pin** 映射为 **PIC 向量**（见 **`irq.c`**）
- **`sysdata`** + 柔性数组 **`priv[0]`**：宿主控制器私有（如 **ECAM 窗口句柄**）

### 3.3 `struct rt_pci_ops`（总线级配置访问）

| 成员 | 作用 |
|------|------|
| **`add`/`remove`** | 子总线挂接/拆除（桥后新 bus） |
| **`map(bus, devfn, reg)`** | 返回 **配置空间** 对应 **MMIO 虚拟指针**（**ECAM** 路径） |
| **`read`/`write`** | 按 **width** 读写配置；可与 **`map` + `rt_pci_bus_read_config_uxx`** 组合 |

### 3.4 `struct rt_pci_bus`

- **链表**：**`list`、`children_nodes`、`devices_nodes`**
- **`parent`**、**`self`（桥设备）或 `host_bridge`（根总线）**
- **`ops`**、**`number`（总线号）**、**`lock`**、**`sysdata`**

### 3.5 `struct rt_pci_device`

继承 **`rt_device`**：**VID/DID/Class、devfn、BAR `resource[]`、IRQ、cap 偏移（PME/MSI/MSIX/PCIe）** 及 **Bus Master/MSI 使能位域** 等。**`RT_PCI_MSI`** 下含 **`msi_pic`、`msi_desc_nodes`、`msix_base`** 等。

### 3.6 `struct rt_pci_driver`

**`ids` + `probe`/`remove`/`shutdown`**，由 **`drivers/core/bus`** 侧 **PCI 总线** 匹配（与 **`rt_pci_device_id`** 宏 **`RT_PCI_DEVICE_ID`/`RT_PCI_DEVICE_CLASS`** 配合）。

### 3.7 辅助头

- **`pci_regs.h`**：**PCIe/PCI 配置空间与能力寄存器偏移/位**（与业界常量一致）
- **`pci_ids.h`**：常见 **Vendor/Device** 枚举（节选）

---

## 4. 配置空间访问（`access.c`）

- 全局 **`struct rt_spinlock rt_pci_lock`**（**`RT_PCI_LOCKLESS`** 时宏为空操作）。
- **`rt_pci_bus_read_config_u8/u16/u32`**：加锁后调 **`bus->ops->read`**；失败时读值填 **全 1**（**`~0`** 语义）。
- **`rt_pci_bus_write_config_*`**：同理 **`ops->write`**。
- **`rt_pci_bus_read_config_uxx`/`write_config_uxx`**：若 **`ops->map`** 非空，则 **`HWREG8/16/32`** 直访映射地址（**ECAM** 默认 **`read`/`write` 即走此路径**）。

---

## 5. ECAM（`ecam.c` / `ecam.h`）

- **`pci_ecam_create(host_bridge, ops)`**：**`rt_calloc`** **`pci_ecam_config_window`**，记录 **`bus_range`、`bus_shift`**，并把 **`host_bridge->ops`** 设为 **`ops->pci_ops`**。
- **`pci_ecam_map`**：按 **(bus−base) << bus_shift | devfn << (bus_shift−8) | reg & mask`** 计算 **ECAM 线性地址**；**`bus_shift==0`** 时用宏 **`PCIE_ECAM_OFFSET`**。
- **`pci_generic_ecam_ops`**：**`map` + `rt_pci_bus_read_config_uxx`/`write_config_uxx`**，即 **MMIO 直读直写** 型 ECAM。

---

## 6. 枚举与 Host 初始化（`probe.c`、`pci.c`）

- **`rt_pci_host_bridge_alloc/free`**：**`rt_calloc`** 含 **`priv`** 尾块。
- **`rt_pci_host_bridge_init`**：若 **`parent.ofw_node`** 存在则 **`rt_pci_ofw_host_bridge_init`**（**`ofw.c`**）。
- **`rt_pci_alloc_device`**：初始化 **链表、subsystem ANY、irq=-1、resource 标记 NONE**；**MSI** 下初始化 **`msi_lock`/`msi_desc_nodes`** 等。
- **`rt_pci_host_bridge_register` → `rt_pci_scan_root_bus_bridge`**：创建 **root bus**、**扫描 slot/桥次级总线**、**`rt_pci_setup_device`**（读头、BAR、capability）、**`rt_pci_assign_irq`**、匹配 **`rt_pci_driver`** 等（**`probe.c`/`pci.c`** 中长篇逻辑，阅读时以函数名搜索）。

**`pci.c`**：**Capability 遍历**（**`pci_find_next_cap_ttl`** 防死循环 **`RT_PCI_FIND_CAP_TTL`**）、**扩展 capability**、**常用配置读写封装**、**`rt_pci_enum_device`** 等工具函数集合。

---

## 7. INTx 中断（`irq.c`）

**`rt_pci_assign_irq`**：若 Host 提供 **`irq_map`**，读 **`INTPIN`**，经 **`irq_slot`（可选 swizzle）** 调 **`irq_map(pdev, slot, pin)`** 得 **Linux 风格 IRQ 号**，写入 **`pdev->irq`** 并 **`PCIR_INTLINE`**。无 **`irq_map`** 则仅打日志，由平台其它路径接 MSI。

---

## 8. Root 桥与电源（`host-bridge.c`、`pme.c`）

- **`host-bridge.c`**：对 **PCI Host Bridge** 设备 **`rt_pci_set_master`**；**`RT_USING_PM`** 时注册 **`rt_pm_device`**，在 **suspend/resume** 里枚举子设备 **`rt_pci_enable_wake`**。
- **`pme.c`**：**PME# / D-state** 与 **`rt_pci_pme_*`、`rt_pci_enable_wake`**（头文件声明，**`pci.h`** 内联 **`rt_pci_pme_capable`**）。

---

## 9. OFW 绑定（`ofw.c`）

在 **`RT_USING_OFW`** 下解析 **`linux,pci-domain`**、**`bus-range`**、**`reg`/`ranges`/`dma-ranges`** 等到 **`rt_pci_host_bridge`**，把 **DT 资源** 填进 **`bus_regions`** 等，供 **generic host** 使用（细节以 **`ofw.c`** 为准）。

---

## 10. MSI 子目录（`msi/`）

**`SConscript`**：**`RT_PCI_MSI`** 时编 **`device.c`、`irq.c`、`msi.c`**。

- **`pci_msi.h`**：**`rt_pci_msi_conf`**、**MSI/MSI-X** 表项与 **API**（分配向量、写 **Message Address/Data**、与 **`rt_pic`** 联动）。
- 实现文件负责：**能力发现、使能/禁用、IRQ domain 注册、MSI-X 表映射** 等（与 **PIC** 紧耦合）。

---

## 11. Endpoint 子目录（`endpoint/`）

**`RT_PCI_ENDPOINT`** 时编 **`endpoint.c`、`mem.c`**。

- **`pci_endpoint.h`**：**`rt_pci_ep_header`、`rt_pci_ep_bar`**、**MSI-X 表描述**、**枚举 `rt_pci_ep_irq`** 等，用于 **SoC 作 EP** 对接 **RC** 的软件模型。

---

## 12. Host 控制器（`host/`）

| 文件 | 作用 |
|------|------|
| **`pci-host-common.c`** | **OFW `compatible`** 为 **`pci-host-ecam-generic`** 等时的 **probe**：**`pci_ecam_create` + `rt_pci_host_bridge_*`** |
| **`pci-host-generic.c`** | **Generic PCI host** 与 **common** 组合注册 |
| **`host/dw/`** | **DesignWare PCIe**：**`pcie-dw_host.c`/`pcie-dw_ep.c`/`pcie-dw.c`** 及 **平台 glue `pcie-dw_platfrom.c`**（文件名拼写以仓库为准） |

**`host/dw/Kconfig`**：控制 **DW PCIe** 主机/EP 功能块是否编译。

---

## 13. 与 DM/PIC 的关系

- **DM**：**`struct rt_pci_device` 继承 `rt_device`**，走统一 **device/driver** 模型。
- **PIC**：**INTx/MSI** 最终需 **可编程中断控制器**；**`RT_USING_PIC`** 为 **OFW `irq.c`** 与 **PCI IRQ** 能力的前提。

---

## 14. 阅读顺序建议

1. **`pci.h`**： **`rt_pci_host_bridge`/`bus`/`device`/`ops`** 与 **`rt_pci_dev_id`**。  
2. **`access.c` + `ecam.c`**：配置空间如何落到 **MMIO**。  
3. **`probe.c`**：**`rt_pci_scan_*`、`rt_pci_setup_device`** 主流程。  
4. **`irq.c` + `host-bridge.c`**：板级 **IRQ 映射** 与 **Root 桥** 特例。  
5. **`msi/*.c`**、**`host/*.c`**：按目标硬件（**generic ECAM** 或 **DW**）深入。

---

## 15. 小结

| 层级 | 说明 |
|------|------|
| 核心 | **总线树 + `rt_pci_ops` 配置访问 + 枚举与驱动匹配** |
| 典型宿主 | **ECAM generic** + **DesignWare** 扩展 |
| 可选 | **MSI/MSI-X、EP 模式、OFW、PME、PM 钩子** |

**`pci.c`/`probe.c` 体量较大**，本文以 **架构与文件职责** 为主；具体 **BAR 分配、桥次级号、资源对齐** 等请以源码中的 **`rt_pci_*` 函数** 为索引继续阅读。
