# RT-Thread 5.2.0 SDIO（SD/MMC/SDIO 主机栈）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/sdio` 目录实现的 **MMC/SD/SDIO 主机软件栈**：以 **`struct rt_mmcsd_host`** 抽象控制器，**`rt_mmcsd_host_ops`** 下发 **`struct rt_mmcsd_req`**；**`mmcsd_detect` 线程** 完成 **上电枚举**（**SDIO → SD → MMC** 顺序）；**`dev_block.c`** 将 **存储卡** 挂到 **块设备（`rt_blk_disk`）**；**`dev_sdio.c`** 实现 **CMD52/CMD53、CCCR/FBR/CIS、功能驱动注册与 SDIO IRQ 线程**。Kconfig 选项名为 **`RT_USING_SDIO`**，实际覆盖 **三类卡协议**。**`rtdevice.h`** 在 **`RT_USING_SDIO`** 下包含 **`dev_mmcsd_core.h`、`dev_sd.h`、`dev_sdio.h`**。

---

## 1. 目录与编译

| 路径/文件 | 作用 |
|-----------|------|
| **`dev_mmcsd_core.c`** | **命令/数据传输框架**、**检测线程**、**host 分配/初始化**、**热插拔邮箱** |
| **`dev_sd.c`** | **SD/SDHC/SDXC** 初始化、**ACMD**、**UHS/切压** 等 |
| **`dev_mmc.c`** | **MMC/eMMC** 初始化与属性配置 |
| **`dev_sdio.c`** | **SDIO I/O**、**CIS 解析**、**`rt_sdio_driver` 匹配**、**IRQ 处理** |
| **`dev_block.c`** | **卡 → `rt_blk_disk`**（读写、忙等待、`SEND_STATUS` 等） |
| **`sdhci/`** | **`RT_USING_SDHCI`**：**标准 SDHCI IP**（**`sdhci.c` + platform + fit-mmc`**） |
| **`Kconfig`** | **`RT_USING_SDIO`**（**`select RT_USING_BLK`**）、线程栈/优先级、**`RT_SDIO_DEBUG`**、**`RT_USING_SDHCI`** |
| **`SConscript`** | 固定编译 **`dev_block.c`…`dev_mmc.c`**；**SDHCI** 追加子目录源文件与 **`sdhci/include`** |

---

## 2. Kconfig 摘要

| 选项 | 含义 |
|------|------|
| **`RT_USING_SDIO`** | 总开关（默认 **n**）；**自动选择 `RT_USING_BLK`** |
| **`RT_SDIO_STACK_SIZE` / `RT_SDIO_THREAD_PRIORITY`** | **SDIO IRQ 线程**（在 **`dev_sdio.c`** 中创建） |
| **`RT_MMCSD_STACK_SIZE` / `RT_MMCSD_THREAD_PRIORITY`** | **`mmcsd_detect` 检测线程** |
| **`RT_MMCSD_MAX_PARTITION`** | **块设备最大分区数**（**`dev_block.c`**） |
| **`RT_SDIO_DEBUG`** | 打开 **`DBG_LOG`** 级别 SDIO 相关日志 |
| **`RT_USING_SDHCI`** | 编译 **SDHCI 控制器驱动** |

---

## 3. 核心数据结构（头文件层次）

### 3.1 `mmcsd_host.h`：`struct rt_mmcsd_host` / `rt_mmcsd_host_ops`

- **`ops->request`**：**唯一必需** 的 **命令+数据路径**（控制器将 **`mmcsd_req_complete(host)`** 在结束时调用，释放 **`sem_ack`**）。
- **`ops->set_iocfg`**：**时钟/电压/总线宽度/timing（Legacy、SD HS、UHS、MMC HS200/HS400 等）**。
- **`ops->get_card_status` / `execute_tuning` / `switch_uhs_voltage` / `enable_sdio_irq`**：可选能力。
- **`card`**：当前枚举到的 **`struct rt_mmcsd_card *`**。
- **`valid_ocr`、`io_cfg`、`freq_min/max`、`flags`**（**SPI 模式、非可移动、SDIO IRQ、HS/HS200/HS400、UHS 等能力位**）。
- **`bus_lock`**：**总线互斥**（**`mmcsd_host_lock/unlock`**）。
- **`sem_ack`**：**与 `mmcsd_send_request` 配对** 的完成同步。
- **SDIO IRQ**：**`sdio_irq_num`、`sdio_irq_sem`、`sdio_irq_thread`** 由 **`dev_sdio.c`** 使用。

### 3.2 `mmcsd_card.h` / `mmcsd_cmd.h`

**CID/CSD/SCR**、**SD Status（UHS 等）**、**`rt_sdio_function`/`rt_mmcsd_card` 内 SDIO 功能数组**、**命令码与标志**（与 **`dev_mmcsd_core.h`** 中 **`rt_mmcsd_cmd`/`rt_mmcsd_req`** 配合）。

### 3.3 `dev_mmcsd_core.h`

**`rt_mmcsd_data`/`cmd`/`req`**、**R1/R5 等状态位宏**、**工具函数声明**（**`mmcsd_send_cmd`、`mmcsd_set_clock` 等**）。

### 3.4 `dev_sdio.h`

**CCCR/FBR 寄存器偏移**、**CIS Tuple 常量**、**`struct rt_sdio_driver`**（**`probe`/`remove`/`id` 表**）、**CMD52/53 封装**、**`init_sdio`、`sdio_register_driver`** 等。

### 3.5 `dev_sd.h` / `dev_mmc.h`

**`init_sd`/`init_mmc`** 及解析 **CSD/扩展寄存器** 的辅助接口声明。

---

## 4. `dev_mmcsd_core.c`：请求路径与枚举线程

### 4.1 **`mmcsd_send_request`**

- 填充 **`cmd`/`data`/`stop` 的反向指针** 与 **`mrq`**。
- 循环 **`host->ops->request`**，**`rt_sem_take(&host->sem_ack)`** 等待完成。
- **`retries`** 递减直至成功或耗尽。

### 4.2 **`mmcsd_send_cmd`**

构造 **无数据** 的 **`rt_mmcsd_req`**，调用 **`mmcsd_send_request`**。

### 4.3 **电源与 I/O 配置**

**`mmcsd_power_up`/`mmcsd_power_off`**、**`mmcsd_select_voltage`**、**`mmcsd_set_iocfg`**（根据 **`host->io_cfg`** 调 **`ops->set_iocfg`**）等，供 **各 `init_*`** 复用。

### 4.4 **`mmcsd_detect` 线程逻辑**（节选）

插入/拔出由 **`mmcsd_change(host)` → `rt_mb_send(mmcsd_detect_mb, host)`** 触发。

- **`host->card == NULL`**（**视为需枚举**）：
  1. **`mmcsd_host_lock`** → **`mmcsd_power_up`** → **`mmcsd_go_idle`**。
  2. **`mmcsd_send_if_cond`**（**SD 接口条件**）。
  3. **`sdio_io_send_op_cond`** 成功则 **`init_sdio`**，失败则继续。
  4. **`mmcsd_send_app_op_cond`** 成功则 **`init_sd`**。
  5. 否则 **`mmc_send_op_cond`** → **`init_mmc`**。
  6. 成功后 **`rt_mb_send(mmcsd_hotpluge_mb, host)`** 通知热插拔监听者。
- **`host->card != NULL`**：**拔出路径** — 若 **`sdio_function_num != 0`** 打日志 **不支持 SDIO 热拔**；否则 **`rt_mmcsd_blk_remove`**、**`rt_free(card)`**、**`card = NULL`**。

### 4.5 **`rt_mmcsd_core_init`**

初始化 **检测邮箱**、**热插拔邮箱**、**`mmcsd_detect` 线程**，并调用 **`rt_sdio_init()`**（**注册 SDIO 驱动链表等**）。**`INIT_PREV_EXPORT`** 尽早运行。

---

## 5. `dev_sd.c` / `dev_mmc.c`

- **`init_sd(host, ocr)`**：**ACMD41** 协商 OCR、**CID/CSD/SCR**、**总线宽与高速模式**、**UHS 切压与 tuning**（依赖 **`host->ops`** 能力位），最终 **`host->card`** 就绪并由 **`dev_block`** 注册磁盘。
- **`init_mmc(host, ocr)`**：**MMC 原生 OCR**、**扩展 CSD**、**HS200/HS400** 等与 **eMMC** 相关的状态机。

---

## 6. `dev_sdio.c`（概要）

- **`sdio_io_rw_direct`**：**CMD52**；**`sdio_io_rw_extended(_block)`**：**CMD53** 多字节/块传输。
- **`init_sdio`**：读 **CCCR**、**使能功能**、**解析 CIS**、**匹配 `rt_sdio_driver`** 调 **`probe`**。
- **`sdio_register_driver`/`sdio_unregister_driver`**：维护 **驱动链表**。
- **中断**：**`sdio_attach_irq`** 与 **`sdio_irq_wakeup`** 配合 **`host->enable_sdio_irq`** 与 **专用线程**（栈/优先级来自 **`RT_SDIO_*`** Kconfig）。

---

## 7. `dev_block.c`：块设备层

- **`struct mmcsd_blk_device`**：**`rt_blk_disk parent` + `struct rt_mmcsd_card *card`**。
- **`rt_hw_blk_disk_register`**（及 **DM `ida`**）生成 **`mmcblk`/`mmcsd` 类设备名**（与 **BSP 命名策略** 相关）。
- **读写**：组 **`READ_MULTIPLE_BLOCK`/`WRITE_MULTIPLE_BLOCK`**（及 **停止/忙检测**），容量与 **`card_blksize`** 对齐。
- **`mmcsd_wait_cd_changed`**：阻塞读 **热插拔邮箱**，返回 **插拔状态**（**`RTM_EXPORT`**）。

---

## 8. `sdhci/`：标准主机控制器

- **`sdhci_host.h`**：**`struct sdhci_host`** 内嵌 **`struct rt_mmcsd_host rthost`**，挂 **SDHCI 寄存器与 DMA 描述符**。
- **`sdhci.c`**：**请求队列、PIO/DMA、IRQ、时钟/电压切换** 等 **Linux SDHCI 风格** 移植层。
- **`sdhci-platform.c`**：**平台设备 + OFW** 绑定 **寄存器/中断**。
- **`fit-mmc.c`**：**Flat Image Tree / 固件描述** 相关适配（**视具体 SoC 使用**）。

启用 **`RT_USING_SDHCI`** 后，**BSP 通常注册 `sdhci_pltfm` 驱动**，在 **probe** 里 **`mmcsd_change`** 触发 **首次枚举**。

---

## 9. BSP 集成流程小结

```text
host = mmcsd_alloc_host();
host->ops = &my_mmcsd_ops;   /* 实现 request + set_iocfg，可选 tuning/sdio_irq */
host->valid_ocr = ...; host->freq_max = ...;
/* 注册到平台/设备模型，插卡或上电后： */
mmcsd_change(host);
```

**`ops->request`** 必须在 **命令完成（含 DATA 结束与错误）** 时调用 **`mmcsd_req_complete(host)`**，否则 **检测线程永久阻塞**。

---

## 10. 小结

| 层次 | 文件 | 职责 |
|------|------|------|
| 协议核心 | **`dev_mmcsd_core.c`** | **REQ/CMD 同步、枚举线程、电源/OCR、热插拔** |
| SD | **`dev_sd.c`** | **SD 卡初始化与高速/UHS** |
| MMC | **`dev_mmc.c`** | **MMC/eMMC** |
| SDIO | **`dev_sdio.c`** | **I/O 卡与功能驱动** |
| 块设备 | **`dev_block.c`** | **`rt_blk_disk`** |
| 控制器 | **`sdhci/*`** | **常见 SDHCI IP** |

目录名 **sdio** 与 **`RT_USING_SDIO`** 在习惯上易理解为 “仅 SDIO”，实际为 **MMC 子系统总入口**；阅读 **`mmcsd_detect`** 可快速建立 **三种卡检测顺序** 的心智模型。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/sdio/`。*
