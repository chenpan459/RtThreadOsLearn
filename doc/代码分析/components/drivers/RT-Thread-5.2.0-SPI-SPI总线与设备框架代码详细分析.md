# RT-Thread 5.2.0 SPI / QSPI（总线与从设备框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/spi` 目录实现的 **SPI 总线驱动框架**：**一条物理总线 `struct rt_spi_bus`（互斥 + 当前占用者 `owner`）** 上挂 **多个逻辑从设备 `struct rt_spi_device`**；底层控制器通过 **`struct rt_spi_ops`** 提供 **`configure` 与 `xfer`**。可选 **QSPI**（**`struct rt_qspi_message`/`rt_qspi_device`**）、**GPIO 模拟软 SPI**、**设备模型（DM）下 OFW 自动扫描与 `rt_spi_driver` 匹配**、以及 **SFUD / SPI SD / SPI 网卡与 Wi-Fi** 等上层设备文件。

**`rtdevice.h`** 在 **`RT_USING_SPI`** 下包含 **`drivers/dev_spi.h`**。

---

## 1. 目录与编译（`SConscript`）

| 源文件 | 条件 | 作用 |
|--------|------|------|
| **`dev_spi_core.c`** | 始终 | **总线注册、挂接从设备、`rt_spi_configure`、锁与 `owner`、`rt_spi_transfer*`、`rt_spi_transfer_message`、`take/release` 总线与 CS** |
| **`dev_spi.c`** | 始终 | **`rt_spi_bus_device_init` / `rt_spidev_device_init`**：把总线/从设备注册为 **`rt_device`**，提供 **`read`/`write`**；**`RT_USING_DM`** 下 **spidev 探测与命名** |
| **`dev_spi_bit_ops.c`**、**`dev_soft_spi.c`** | **`RT_USING_SOFT_SPI`** | **位bang HAL + 多组 Kconfig 软 SPI 总线** |
| **`dev_qspi_core.c`** | **`RT_USING_QSPI`** | **`rt_qspi_bus_register`、`rt_qspi_configure`、`rt_qspi_transfer_message` 等** |
| **`dev_spi_dm.c`**、**`dev_spi_bus.c`** | **`RT_USING_DM`** | **OFW 解析从节点、`spi_bus_scan_devices`、`rt_spi_driver_register`、总线匹配与 probe** |
| **`dev_spi_flash_sfud.c` + `sfud/src/*.c`** | **`RT_USING_SFUD`** | **SFUD 与块设备封装**（**`-std=c99`/`--c99`** 由平台追加） |
| **`dev_spi_msd.c`** | **`RT_USING_SPI_MSD`** | **SPI 模式 SD/TF** |
| **`enc28j60.c`** | **`RT_USING_ENC28J60`** | **ENC28J60 SPI 以太网** |
| **`dev_spi_wifi_rw009.c`** | **`RT_USING_SPI_WIFI`** | **RW009/007 SPI Wi-Fi** |

**`Kconfig`**：总开关 **`RT_USING_SPI`**；子项包括 **软 SPI0–6 管脚与延时**、**`RT_SPI_BITOPS_DEBUG`**、**`RT_USING_QSPI`**、**`RT_USING_SPI_MSD`**、**`RT_USING_SFUD`**（含 **SFDP、片内表、QSPI、默认 `max_hz`**）、**`RT_USING_ENC28J60`**、**`RT_USING_SPI_WIFI`** 等。

---

## 2. 核心数据结构与模式位（`dev_spi.h`）

### 2.1 `struct rt_spi_bus`

- **`struct rt_device parent`**：注册为 **`RT_Device_Class_SPIBUS`**。
- **`mode`**：**`RT_SPI_BUS_MODE_SPI`** 或 **`RT_SPI_BUS_MODE_QSPI`**（**`rt_qspi_bus_register`** 在 **`rt_spi_bus_register` 成功后** 置 **`QSPI`**）。
- **`const struct rt_spi_ops *ops`**。
- **`struct rt_mutex lock`**：**全总线互斥**，所有 **`rt_spi_transfer*` / `transfer_message`** 路径先 **`rt_mutex_take`**。
- **`struct rt_spi_device *owner`**：**当前已 `configure` 并占用总线的从设备**；切换从设备时 **`ops->configure(device, &device->config)`** 并更新 **`owner`**。

**`RT_USING_DM`** 下额外：**`cs_pins[]` / `cs_active_vals[]`、`slave`、`num_chipselect`**，供 **DT CS 与片选极性** 使用；**`rt_spi_bus_register`** 末尾调用 **`spi_bus_scan_devices(bus)`**。

### 2.2 `struct rt_spi_device`

- **`struct rt_device parent`**：**`RT_Device_Class_SPIDevice`**。
- **`struct rt_spi_bus *bus`**。
- **`struct rt_spi_configuration config`**：**`mode`（CPOL/CPHA/MSB/CS 等）、`data_width`、`max_hz`**；DM 下 **`data_width_tx/rx`**。
- **`rt_base_t cs_pin`**：非 DM 或 GPIO CS 时常用 **`PIN_NONE`** 表示由控制器或 **`NO_CS`** 处理。

**`RT_USING_DM`**：**`name`、`id`、`ofw_id`、`chip_select[]`、`cs_setup/hold/inactive` 延时**。

### 2.3 `struct rt_spi_ops`

| 回调 | 职责 |
|------|------|
| **`configure(device, configuration)`** | 按 **该从设备** 的 **时钟/模式/位宽** 配置硬件控制器 |
| **`xfer(device, message)`** | 执行 **单段 `rt_spi_message`**（含 **可选 `next` 链** 由上层循环或 HAL 自行展开——框架里 **`rt_spi_transfer_message` 对 `next` 逐段调用 `xfer`**） |

### 2.4 `struct rt_spi_message`

- **`send_buf` / `recv_buf` / `length`**。
- **`cs_take` / `cs_release`**：片选 **拉低/拉高** 语义由 **`xfer` 实现**配合 **`RT_SPI_CS_HIGH`/`NO_CS`**。
- **`next`**：组成 **分段传输链**。

### 2.5 QSPI

- **`struct rt_qspi_configuration`**：在 **`rt_spi_configuration`** 基础上增加 **`medium_size`、`ddr_mode`、`qspi_dl_width`**。
- **`struct rt_qspi_message`**：扩展 **instruction / address / alternate_bytes / dummy_cycles / qspi_data_lines**；**`rt_qspi_transfer_message`** 最终仍调用 **`bus->ops->xfer(&device->parent, &message->parent)`**（**控制器需在 `xfer` 内识别 QSPI 消息布局**）。

---

## 3. `dev_spi_core.c`：业务逻辑摘要

### 3.1 `rt_spi_bus_register`

- **`rt_spi_bus_device_init`** 注册总线设备。
- **`rt_mutex_init(&bus->lock, name, ...)`**，**`bus->ops = ops`**，**`owner = RT_NULL`**，**`mode = RT_SPI_BUS_MODE_SPI`**。
- **DM**：按 **命名管脚 `cs`** 填充 **`cs_pins[]`**；失败时 **unregister**；最后 **`spi_bus_scan_devices`**。

### 3.2 `rt_spi_bus_attach_device_cspin`

- **`rt_device_find(bus_name)`** 且类型为 **`RT_Device_Class_SPIBUS`**。
- 若 **`owner == RT_NULL`**，将 **`owner` 预置为当前 device**（首从设备占位策略）。
- **`rt_spidev_device_init`**、**`cs_pin` 配置为输出**（非 **`PIN_NONE`**）、清零 **`config`**、保存 **`user_data`**。

### 3.3 `rt_spi_configure` 与 `rt_spi_bus_configure`

- **`rt_spi_configure`**：按 **CS 极性** 写 **GPIO 空闲电平**；若 **与旧配置相同** 直接 **`RT_EOK`**；否则更新 **`device->config`** 并调用 **`rt_spi_bus_configure`**。
- **`rt_spi_bus_configure`**：在 **持有 `bus->lock`** 前提下，仅当 **`owner == device`** 时调用 **`ops->configure`**；否则返回 **`-RT_EBUSY`**（表示 **配置已写入设备，待其独占总线后生效**）。

### 3.4 传输 API 的共性

**`rt_spi_transfer` / `send_then_recv` / `send_then_send`** 均：

1. **`rt_mutex_take(&bus->lock)`**。
2. 若 **`owner != device`**：**`ops->configure`**，成功则 **`owner = device`**。
3. 构造 **`rt_spi_message`**（**`cs_take`/`cs_release`** 按场景置位），调用 **`ops->xfer`**。
4. **`rt_mutex_release`**。

**`rt_spi_transfer_message`**：对 **`message` 链表** **while 循环** 逐段 **`xfer`**；任一段 **`result < 0`** 则 **break**，返回 **当前 `index`（失败段）**；**全部成功则 `index` 最终为 `RT_NULL`**（与头文件 **“`RT_NULL` 表示成功”** 一致）。

### 3.5 总线 / 片选 `take` / `release`

- **`rt_spi_take_bus` / `rt_spi_release_bus`**：**只操作互斥与 `configure/owner`**，**`release_bus` 要求 `owner == device`**。
- **`rt_spi_take` / `rt_spi_release`**：构造 **`length==0`** 且仅 **`cs_take` 或 `cs_release`** 的 **message**，调用 **`xfer`**——**具体 CS 时序完全依赖 HAL 的 `xfer` 实现**。

---

## 4. `dev_spi.c`：设备对象与 DM spidev

- **总线 `read/write`**：转发到 **`rt_spi_transfer(bus->owner, ...)`**，因此 **仅当已设置 `owner` 且其已 open/配置** 才有意义（历史兼容 **0.3.x/1.0.x** 风格）。
- **从设备 `read/write`**：**`rt_spi_transfer(device, ...)`**。
- **DM**：若 **compatible 为 `spidev`** 则 **拒绝**（**`LOG_E` Linux spidev 语义不在 OFW 支持**）；否则设备名 **`{bus_name}_{chip_select[0]}`**。

---

## 5. DM：`dev_spi_bus.c` 与 `dev_spi_dm.c`

- **`spi_bus_scan_devices`**：遍历 **SPI 控制器 OFW 子节点**，分配 **`rt_spi_device`**，**`spi_device_ofw_parse`** 后 **`rt_spi_device_register`**（挂到 **`rt_bus` 上的 `spi_bus`**）。
- **`rt_spi_driver_register`**：将 **`struct rt_spi_driver`** 注册到 **`spi_bus`**。
- **`spi_match`**：**`ids` 名字表** 或 **`ofw_ids` + `rt_ofw_node_match`**。
- **`spi_device_ofw_parse`**：解析 **`spi-cpha/cpol/3wire/lsb-first/cs-high`**、**`spi-tx/rx-bus-width`**、**`reg` 片选数组**、**`cs-setup/hold/inactive` 延时** 等（与 Linux DT 习惯对齐）。

---

## 6. `dev_qspi_core.c`

- **`rt_qspi_bus_register`**：**`rt_spi_bus_register` 成功后** **`bus->mode = RT_SPI_BUS_MODE_QSPI`**。
- **`rt_qspi_configure`**：比较 **QSPI 扩展字段**，更新后 **`rt_spi_bus_configure(&device->parent)`**。
- **`rt_qspi_transfer_message`**：**加锁 → owner/configure → `xfer(&message->parent)`**；若 **`xfer` 返回 0**，则 **`rt_set_errno(-RT_EIO)`**（与源码一致，调用方需注意与 **`rt_spi_transfer`** 返回长度的区别）。

---

## 7. 软 SPI（`dev_spi_bit_ops.c` / `dev_soft_spi.c`）

- **`RT_USING_SOFT_SPI`**：**`select RT_USING_PIN`**。
- **位操作 + 延时** 实现 **`configure`/`xfer`**，多路 **`RT_USING_SOFT_SPI0`…`SPI6`** 由 **Kconfig 管脚与 `TIMING_DELAY`** 实例化独立总线。

---

## 8. 可选设备与 SFUD

| 组件 | 说明 |
|------|------|
| **`sfud/`** | 第三方 **SFUD** 库源码（**`sfud.h`、`sfud.c`、`sfud_sfdp.c`** 等） |
| **`dev_spi_flash_sfud.c`** | 把 **SFUD** 挂到 **`struct rt_spi_device`/`rt_qspi_device`**，注册 **块设备** 与 **`RT_DEVICE_CTRL_BLK_*`** |
| **`dev_spi_msd.c`** | **DFS** 下的 **SPI SD** |
| **`enc28j60.c` / `dev_spi_wifi_rw009.c`** | **LwIP** 依赖的 **SPI 网卡/Wi-Fi** |

---

## 9. 驱动编写要点

1. **实现 `rt_spi_ops`**：**`configure` 必须** 根据 **`struct rt_spi_configuration`** 切换 **波特率、模式、位宽**；**`xfer` 必须** 处理 **`cs_take/cs_release`** 与 **全双工/只发/只收**（**`send_buf`/`recv_buf` 可一方为 `NULL`**）。
2. **多从设备**：依赖框架的 **`owner` + `mutex`**；切换从设备时会 **自动 `configure`**。
3. **分段传输**：优先 **`rt_spi_transfer_message` + `next` 链**，保证 **中间段不释放 CS**（与 **`send_then_recv`** 内建两段消息等价）。
4. **QSPI**：**`xfer` 内解析 `struct rt_qspi_message`**（通过 **`message` 实际类型** 或约定），**`rt_qspi_transfer_message` 不单独拆分阶段**。

---

## 10. 小结

| 层次 | 文件 | 职责 |
|------|------|------|
| **API 与锁** | **`dev_spi_core.c`** | **总线/设备挂接、配置缓存、`transfer*`、`transfer_message`、take/release** |
| **设备注册** | **`dev_spi.c`** | **`rt_device` 层 read/write**、**DM spidev 策略** |
| **DT 与驱动模型** | **`dev_spi_dm.c`、`dev_spi_bus.c`** | **OFW 解析、扫描、`rt_spi_driver` 匹配** |
| **QSPI** | **`dev_qspi_core.c`** | **QSPI 配置与传输封装** |
| **软 SPI + 上层** | **其余可选 `.c`** | **GPIO SPI、Flash、SD、网络** |

该目录是 RT-Thread **SPI 主从通信的核心框架**；具体 SoC 的 **硬件 SPI 驱动** 通常在 **BSP/`libraries/HAL`** 中 **`rt_spi_bus_register`** 接入本框架。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/spi/`。*
