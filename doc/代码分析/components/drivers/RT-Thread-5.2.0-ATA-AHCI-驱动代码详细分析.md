# RT-Thread 5.2.0 ATA（AHCI）驱动代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/ata` 目录下的实现。该目录在 5.2.0 中**仅实现 SATA 的 AHCI 模式**，通过 **SCSI 中间层** 对上暴露块设备能力，而不是传统 IDE 并行 ATA 控制器驱动。

涉及文件：

- 核心实现：`rt-thread-5.2.0/components/drivers/ata/ahci.c`
- PCI 绑定：`rt-thread-5.2.0/components/drivers/ata/ahci-pci.c`
- 构建与配置：`SConscript`、`Kconfig`
- 寄存器与数据结构：`rt-thread-5.2.0/components/drivers/include/drivers/ahci.h`（由 `rtdevice.h` 在 `RT_ATA_AHCI` 打开时包含）

---

## 1. 模块定位与依赖关系

### 1.1 在系统中的角色

整体链路可概括为：

```text
应用 / 文件系统
    ↓
块设备（BLK，依赖 RT_USING_BLK）
    ↓
SCSI 子系统（RT_USING_SCSI）
    ↓
本模块：AHCI Host → 端口 → FIS + PRDT → SATA 设备
    ↓
硬件：AHCI HBA（通常挂在 PCI/PCIe）
```

`Kconfig` 中 `RT_USING_ATA` 依赖 **`RT_USING_DM`（设备模型）**、**`RT_USING_BLK`**、**`RT_USING_DMA`**。子选项 `RT_ATA_AHCI` 额外依赖 **`RT_USING_SCSI`**，说明 AHCI 路径把磁盘当成 **SCSI 目标** 来管理命令与容量查询。

`RT_ATA_AHCI_PCI` 在开启 **`RT_USING_PCI`** 时编译 `ahci-pci.c`，提供标准 PCI AHCI 设备的 probe/remove。

### 1.2 与 `ahci.h` 的分工

- **`ahci.h`**：AHCI 1.x 风格寄存器偏移与位域、PRDT/SG 上限、FIS/ATA 命令号、`struct rt_ahci_host` / `struct rt_ahci_port`、可选回调 `struct rt_ahci_ops`，以及 Identify 数据的内联解析辅助函数。
- **`ahci.c`**：HBA 初始化、端口链路/DMA 布置、中断服务、将 SCSI 操作码映射为 ATA 命令与 DMA 传输。
- **`ahci-pci.c`**：把 `struct rt_pci_device` 映射到 `struct rt_ahci_host`，处理 BAR、MSI/INTx、厂商 quirk，并调用 `rt_ahci_host_register()`。

---

## 2. 关键数据结构

### 2.1 `struct rt_ahci_host`（`ahci.h`）

继承 **`struct rt_scsi_host parent`**，因此 AHCI 控制器在框架里表现为 **SCSI Host**。

主要字段：

| 字段 | 含义 |
|------|------|
| `irq` / `regs` | 全局中断号、HBA MMIO 基址 |
| `ports_nr` / `ports_map` | 逻辑端口数（由 CAP.NP 推导）、已实现端口位图（可读 `HBA_PI`，也可被设备树属性 `ports-implemented` 覆盖） |
| `ports[32]` | 每端口运行时状态（寄存器影子指针、DMA 池、完成量等） |
| `cap` | 保存/处理后的 HBA CAP 能力位（代码中会屏蔽并写回部分位） |
| `max_blocks` | 单次 ATA 命令最大块数，注册时若为 0 则默认 **0x80** |
| `ops` | 平台/PCI 可选钩子（host/port 初始化、链路、DMA、端口 ISR） |

### 2.2 `struct rt_ahci_port`

| 字段 | 含义 |
|------|------|
| `regs` | 该端口寄存器块基址：`host->regs + 0x100 + i * 0x80` |
| `dma` / `dma_handle` | `rt_dma_alloc_coherent()` 分配的一体化 DMA 区 |
| `cmd_slot` | 命令列表（本实现使用 **命令槽 0** 对应的区域） |
| `rx_fis` | 接收 FIS 缓冲区（256 字节对齐需求由 `RT_AHCI_RX_FIS_SIZE` 体现） |
| `cmd_tbl` / `cmd_tbl_dma` / `cmd_tbl_sg` | 命令表头 + PRDT（S/G）区 |
| `int_enabled` | 写入 `PORT_INTE` 的中断使能集合 |
| `block_size` | 逻辑块大小：INQUIRY 后根据 **端口签名** 设为 512（磁盘）或 2048（光驱类签名） |
| `ataid` | Identify Device 数据（512 字节，256 个 `uint16`），按需分配 |
| `link` | 链路是否就绪 |
| `done` | **`rt_completion`**，与 `ahci_request_io()` 中 `rt_completion_wait()` 配对 |

### 2.3 `struct rt_ahci_ops`

BSP 或 PCI quirk 可挂接：

- `host_init`：HBA 级初始化（如 Intel/JMicron 配置空间修补）。
- `port_init` / `port_link_up` / `port_dma_init`：端口级；若未提供 `port_link_up`，核心代码用 **SSTS.DET** 轮询等待 **PHYRDY**。
- `port_isr`：端口中断的额外处理；无论是否实现，ISR 里都会对已连接端口 **`rt_completion_done(&port->done)`**。

### 2.4 DMA 布局常量（`ahci.h`）

- `RT_AHCI_MAX_SG`：**56** 个 PRDT 项。
- `RT_ACHI_PRDT_BYTES_MAX`：单段最大 **4 MiB**（注意宏名拼写为 `ACHI`）。
- `RT_AHCI_DMA_SIZE`：单端口 DMA 池总大小 = 命令列表区 + 命令表 + RX FIS 等（与 `ahci.c` 中 `rt_memset(dma, 0, RT_AHCI_DMA_SIZE)` 一致）。

`ahci.c` 中在 `cmd_slot` 之后预留 **`RT_AHCI_CMD_SLOT_SIZE + 224`** 再放置 `rx_fis`，与「仅使用槽 0」的简化布局相匹配。

---

## 3. 构建系统（`SConscript`）

- 未定义 `RT_USING_ATA` 时整个组为空。
- `RT_ATA_AHCI` 时编译 `ahci.c`。
- 同时 `RT_ATA_AHCI_PCI` 时增加 `ahci-pci.c`。
- `CPPPATH` 指向 `components/drivers/include`，从而可 `#include <drivers/ahci.h>`。

---

## 4. `rt_ahci_host_register()` 初始化流程（`ahci.c`）

可归纳为以下阶段（与源码顺序一致）：

1. **参数检查**：`host`、`host->parent.dev`、`host->ops` 非空；`max_blocks` 默认化。
2. **HBA 复位**：置 `GHC.RESET`，最多 5 次 × 200 ms 等待自清。
3. **使能 AHCI 模式**：写 `GHC.AHCI_EN`；读 `CAP`，按位清除后再写回部分能力位（仅保留 SPM/SSS/SIS 相关），再读回作为有效 `host->cap`；向 `HBA_PI` 写 **0xf**（固件实现的端口掩码相关操作）。
4. **`ops->host_init`**：可选；失败则 `_fail`。
5. **端口数与位图**：`ports_nr = (CAP & NP) + 1`；`ports_map` 来自 `HBA_PI`，并可被设备树 **`ports-implemented`** 属性覆盖。
6. **对每个实现位为 1 的端口**：
   - 计算 `port->regs`。
   - **停止端口 DMA**：若 `CMD` 中 LIST_ON/FIS_ON/FIS_RX/START 等置位，则清除并 **delay 500 ms**。
   - **`ops->port_init`**（可选）。
   - 置 **SPIN_UP** 等，建立链路：`ops->port_link_up` 或默认轮询 `SSTS.DET == PHYRDY`。
   - 清理 **`SERR`**、等待 **`TFD`** 非 BSY/DRQ（带重试）；若 `DET == COMINIT` 则 **`--i` 重试该端口**。
   - 置 **`port->link = RT_TRUE`**（在 PHYRDY 条件下）。
7. **全局中断使能**：`GHC.IRQ_EN`。
8. **对有链路的端口分配 coherent DMA**，布局：`cmd_slot` → `rx_fis` → `cmd_tbl`（含 SG 区）；写 **`CLB`/`CLBU`、`FB`/`FBU`**；可选 **`ops->port_dma_init`**；置 **`CMD`**（ACTIVE、FIS_RX、POWER_ON、SPIN_UP、START）；**忙等待 TFD.BSY** 清除（最长约 20 s）；写 **`PORT_INTE`**；**`rt_completion_init(&port->done)`**。
9. **注册 IRQ**：`rt_hw_interrupt_install` + `rt_hw_interrupt_umask`；填充 **`scsi->max_lun`、`max_id`、`ops = &ahci_scsi_ops`**；**`rt_scsi_host_register()`**。

失败路径会 mask 中断、`rt_pic_detach_irq`，并返回错误。

### 4.1 `rt_ahci_host_unregister()`

反注册 SCSI Host，mask/detach 中断，释放每端口 `ataid` 与 DMA，关闭 `GHC` 的 AHCI/IRQ 使能。

---

## 5. 中断与 I/O 完成路径

### 5.1 `ahci_isr()`

1. 读 **`HBA_INTS`** 得到端口位图，对置位端口循环。
2. 读 **`PORT_INTS`**；若 `port->link` 为真：
   - 可选调用 **`host->ops->port_isr`**；
   - **`rt_completion_done(&port->done)`**（与同步提交的命令完成模型对应）。
3. 写回清除 **`PORT_INTS`**、**`HBA_INTS`**。

因此 **`ahci_request_io()`** 通过 **`HWREG32_FLUSH(PORT_CI, 1)`** 下发 **命令槽 0** 后，依赖 **SG 完成等事件** 触发中断并唤醒 completion。

### 5.2 `ahci_request_io()` 要点

- 检查 **`SSTS.DET == PHYRDY`**，否则 `-RT_EIO`。
- **`ahci_fill_sg()`**：把缓冲区按 4 MiB 切段填入 PRDT；虚拟地址经 **`rt_kmem_v2p`** 转为物理地址；若主机无 **64 位 DMA** 能力而地址超出 4G，返回失败。
- 将 **H2D Register FIS** 拷入 **`cmd_tbl` 首部**，**`ahci_fill_cmd_slot()`** 设置 **`opts`**（含 FIS  DWORD 长度、PRDT 个数、写位置 **W** 位）。
- **写路径**：**`rt_hw_cpu_dcache_ops(FLUSH, buffer, size)`**。
- 置 **`CI` bit 0** 启动命令；**`rt_completion_wait(..., 10 s)`**。
- **读路径成功后**：**`INVALIDATE`** 数据缓存。

---

## 6. SCSI → ATA 映射（`ahci_scsi_transfer`）

`static struct rt_scsi_ops ahci_scsi_ops` 仅实现 **`.transfer`**。

### 6.1 读写

- **READ10/WRITE10、READ12/WRITE12、READ16/WRITE16**：LBA 与长度从 SCSI CDB 解析（大端），调用 **`ahci_scsi_cmd_rw()`**。
- **`ahci_scsi_cmd_rw()`** 使用 **READ/WRITE EXT**（`RT_AHCI_ATA_CMD_READ_EXT` / `WRITE_EXT`），按 **`host->max_blocks`** 分段，构造 **20 字节 Register FIS**（含 **48 位 LBA** 与 **16 位 sector count**），循环调用 **`ahci_request_io()`**。

### 6.2 同步缓存

- **SYNCHRONIZE_CACHE10/16**：根据 Identify 能力选择 **FLUSH** 或 **FLUSH EXT**；命令表与槽配置方式与读写类似，但无数据缓冲区，completion 超时 **5 s**。

### 6.3 TRIM（WRITE SAME）

- **WRITE_SAME10/16**：意图映射为 ATA **DATA SET MANAGEMENT**（feature **TRIM**），见 **`ahci_scsi_cmd_write_same()`**。

### 6.4 容量与探测

- **READ_CAPACITY10/16**：在已有 **`ataid`** 前提下，用 **`rt_ahci_ata_id_n_sectors()`** 与 **`port->block_size`** 填应答；10 字节格式若容量过大则将 last block 钳位到 **0xffffffff**。
- **TEST_UNIT_READY**：仅检查 **`ataid`** 是否已分配。
- **INQUIRY**：若尚无 **`ataid`** 则分配缓冲区，发 **IDENTIFY DEVICE**，再 **小端转 CPU** 并交换字节序取出型号、固件版本；**vendor** 固定为 **`"ATA     "`**；根据 **`PORT_SIG`** 区分磁盘与 **CDROM 签名**，设置 **`devtype`** 与 **`block_size`**（512 / 2048）。
- **REQUEST_SENSE**：返回简化的 sense 头（**`error_code = 0x72`**），不查硬件 sense 缓存。
- **MODE_SENSE / MODE_SELECT**：显式 **`-RT_ENOSYS`**。

---

## 7. PCI 驱动层（`ahci-pci.c`）

### 7.1 `struct pci_ahci_quirk`

可指定：

- **`bar_idx` + `bar_offset`**：是否不用默认 **BAR5**（`AHCI_REG_BAR`），例如 Cavium 使用 **BAR0**。
- **`ops`**：专用 **`rt_ahci_ops`**（如 Intel 在 config space **0x92** 上的特殊序列）。

### 7.2 `pci_ahci_probe()`

1. **`rt_calloc`** 分配 **`struct pci_ahci_host`**。
2. **`rt_pci_iomap`** 映射 MMIO；设置 **`ahci->ops`**（quirk 或默认 **`pci_ahci_ops`**）。
3. **`rt_pci_msi_enable`** 成功则记 **`is_msi`**；否则 **`rt_pci_irq_unmask`**；**`ahci->irq = pdev->irq`**。
4. **`rt_pci_set_master`** 后调用 **`rt_ahci_host_register()`**。
5. 成功则将 **`pdev->parent.user_data`** 指向 **`pci_ahci`**。

### 7.3 厂商特殊逻辑

- **JMicron**：`pci_ahci_init` 写 config **`0x41 = 0xa1`**。
- **Intel**：`pci_ahci_intel_init` 对 **`0x92`** 做 **低 4 位清再置位** 的 toggling（常见为端口映射/复位相关 workaround）。
- **设备 ID 表**：包含 Intel **0x2922**、ASMedia、Marvell、Cavium **0xa01c**，以及 **PCI class SATA AHCI** 通配项。

### 7.4 `pci_ahci_remove()` / `shutdown`

调用 **`rt_ahci_host_unregister()`**；若 MSI 则 **`rt_pci_msix_disable`**（与 probe 中 **`rt_pci_msi_enable`** 命名上不对等，阅读内核/RT-Thread PCI API 时建议对照确认是否应为 **`rt_pci_msi_disable`**）；清除 bus master、**`rt_iounmap`**、释放结构体。

---

## 8. 阅读源码时的注意点

1. **单命令槽模型**：下发命令固定使用 **`PORT_CI` bit 0** 与第一套命令表，适合理解 AHCI 流程；高并发或 NCQ 未在此展开。
2. **缓存一致性**：依赖 **`rt_hw_cpu_dcache_ops`**，在 DMA 非一致性内存或带 D-cache 的 CPU 上必须正确实现。
3. **`ahci_scsi_cmd_write_same()`**：当前实现中在设置本地 **`fis[]`** 之后**未**像 **`ahci_scsi_synchronize_cache()`** 那样执行 **`rt_memcpy(port->cmd_tbl, fis, ...)`** 与 **`ahci_fill_cmd_slot()``**，随后直接写 **`PORT_CI`**；若需依赖 **WRITE SAME → TRIM** 路径，建议对照 **`ahci_request_io()`** 与 **`ahci_scsi_synchronize_cache()`** 自行核对逻辑是否完整。
4. **PCI remove 与 MSI**：`probe` 使用 **`rt_pci_msi_enable`**，`remove` 分支调用 **`rt_pci_msix_disable`**，与常见 **`msi_enable` / `msi_disable` 成对** 的写法不一致，审阅时可作为维护项。
5. **宏命名**：`RT_ACHI_PRDT_BYTES_MAX` 与注释 **`ACHI`** 为历史拼写，与 **`AHCI`** 混用，搜索时两种都试。

---

## 9. 小结

`components/drivers/ata` 在 RT-Thread 5.2.0 中提供的是 **AHCI SATA Host + PCI 绑定**，通过 **`rt_scsi_host`** 接入 **SCSI/块设备栈**。核心文件 **`ahci.c`** 完成 HBA/端口/DMA/中断与 **SCSI 命令到 ATA FIS** 的转换；**`ahci-pci.c`** 负责 **PCI 资源与厂商 quirk**；**`ahci.h`** 集中定义寄存器与数据结构，便于非 PCI 场景复用 **`rt_ahci_host_register()`** 实现 SoC 内置 AHCI。

若需在 BSP 上启用，需在 menuconfig 中打开 **`RT_USING_ATA`** 及链路依赖（**DM、BLK、DMA、SCSI**），PCI 机型再开 **`RT_ATA_AHCI_PCI`**，并保证 **PCI 与中断控制器** 与 **DMA 一致性 API** 已正确对接。
