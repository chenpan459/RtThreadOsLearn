# RT-Thread 5.2.0 I2C 总线设备框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/i2c` 目录实现的 **I2C 总线抽象**：**`struct rt_i2c_bus_device`** 作为 **`RT_Device_Class_I2CBUS`** 设备对外暴露 **`read`/`write`/`control`**，内核路径以 **`rt_i2c_transfer`** 为核心，经 **`rt_i2c_bus_device_ops`** 落到 BSP 硬件或 **GPIO 位bang**。在 **`RT_USING_DM`** 下扩展 **总线模型**（**`rt_bus`**）、**`rt_i2c_client` / `rt_i2c_driver`** 及设备树时序解析。

涉及文件：

- 核心 API 与类型：`components/drivers/include/drivers/dev_i2c.h`（`rtdevice.h` 在 **`RT_USING_I2C`** 下包含）
- 位操作扩展头：`dev_i2c_bit_ops.h`（**`RT_USING_I2C_BITOPS`**）
- DM 扩展头：`dev_i2c_dm.h`（**`RT_USING_DM`** 且 **`RT_USING_I2C`**）
- 实现：`dev_i2c_core.c`、`dev_i2c_dev.c`、`dev_i2c_bit_ops.c`、`dev_soft_i2c.c`、`dev_i2c_bus.c`、`dev_i2c_dm.c`
- 构建：`Kconfig`、`SConscript`

---

## 1. 模块定位与分层

```text
传感器/外设驱动（rt_i2c_transfer / rt_i2c_master_send|recv）
    ↓
rt_mutex（总线互斥，在 rt_i2c_transfer 内）
    ↓
rt_i2c_bus_device_ops.master_xfer（+ 可选 i2c_bus_control）
    ↓
硬件 I2C 控制器驱动 或  dev_i2c_bit_ops（GPIO 时序）
```

**字符设备语义**：`dev_i2c_dev.c` 将 **`rt_device`** 的 **`read`/`write`** 映射为 **`rt_i2c_master_recv`/`send`**，其中 **`pos`** 低 16 位为 **7/10 位从机地址**，高 16 位为 **`flags`**（与 **`struct rt_i2c_msg.flags`** 同命名空间）。

---

## 2. Kconfig 与 SConscript

| 选项 | 含义 |
|------|------|
| **`RT_USING_I2C`** | 总开关；`SConscript` 中 **`depend = ['RT_USING_I2C']`** |
| **`RT_I2C_DEBUG`** | 打开 **`dev_i2c_core.c` / `dev_i2c_dev.c`** 更详细日志 |
| **`RT_USING_I2C_BITOPS`** | 编译 **`dev_i2c_bit_ops.c`**；默认 **y** |
| **`RT_I2C_BITOPS_DEBUG`** | 位bang 路径调试日志 |
| **`RT_USING_SOFT_I2C`** | 编译 **`dev_soft_i2c.c`**；**`select RT_USING_PIN`**、**`select RT_USING_I2C_BITOPS`**；Kconfig 下可配置 **I2C0～I2C8** 的 SCL/SDA 引脚、总线名、**`timing_delay`（μs）**、**`timing_timeout`（tick）** |

**`SConscript`** 固定：**`dev_i2c_core.c`、`dev_i2c_dev.c`**；条件追加 **`dev_i2c_bit_ops.c`**、**`dev_soft_i2c.c`**；**`RT_USING_DM`** 时再追加 **`dev_i2c_bus.c`、`dev_i2c_dm.c`**。

---

## 3. 关键数据结构（`dev_i2c.h`）

### 3.1 `struct rt_i2c_msg`

- **`addr`**：从机地址（7 位语义由 **`master_xfer`** 解释；10 位见 **`RT_I2C_ADDR_10BIT`**）。
- **`flags`**：读写及总线行为位（见下节）。
- **`len` / `buf`**：长度与缓冲区。

### 3.2 消息 flags（节选）

| 宏 | 含义 |
|----|------|
| **`RT_I2C_WR` / `RT_I2C_RD`** | 写 / 读 |
| **`RT_I2C_ADDR_10BIT`** | 10 位寻址 |
| **`RT_I2C_NO_START`** | 本段前不发送 **START**（多段组合传输） |
| **`RT_I2C_IGNORE_NACK`** | 忽略从机 NACK；位bang 下 **不重试地址** |
| **`RT_I2C_NO_READ_ACK`** | 读时主机不发 ACK/NACK（最后字节等场景） |
| **`RT_I2C_NO_STOP`** | 本段结束不发送 **STOP** |

### 3.3 `struct rt_i2c_bus_device_ops`

| 成员 | 说明 |
|------|------|
| **`master_xfer(bus, msgs, num)`** | 主机模式传输；**`rt_i2c_transfer`** 唯一强依赖 |
| **`slave_xfer`** | 从机模式（框架内默认未在 core 中统一封装调用，由驱动自选） |
| **`i2c_bus_control(bus, cmd, args)`** | 总线控制；**`rt_i2c_control`** 及 **`dev_i2c_dev`** 中未内置处理的 **`control`** 会转发到此 |

### 3.4 `struct rt_i2c_bus_device`

- **`parent`**：**`rt_device`**。
- **`ops`**：总线操作表。
- **`flags`**：如 **`RT_I2C_DEV_CTRL_10BIT`** 置 **`RT_I2C_ADDR_10BIT`**（由 **`control`** 路径设置，供应用/驱动约定使用）。
- **`lock`**：**`rt_mutex`**，在 **`rt_i2c_bus_device_register`** 中初始化。
- **`timeout`**：默认若为 0 则置 **`RT_TICK_PER_SECOND`**（供位bang 等使用）。
- **`retries`**：位bang 发地址失败时的重试次数（**`RT_I2C_IGNORE_NACK`** 时为 0）。
- **`priv`**：驱动私有；位bang 下为 **`struct rt_i2c_bit_ops *`**。

### 3.5 `rt_i2c_priv_data`

配合 **`RT_I2C_DEV_CTRL_RW`**：**`msgs` + `number`** 一次 **`rt_i2c_transfer`**。

### 3.6 DM：`struct rt_i2c_client` / `struct rt_i2c_driver`

- **`client`**：绑定 **`bus`、`client_addr`**；在 OFW 下含 **`parent`（device）、`ofw_node`、`name`** 等。
- **`driver`**：继承 **`rt_driver`**，提供 **`ids`（名字表）/ `ofw_ids`**、**`probe`/`remove`/`shutdown`**。
- **`rt_i2c_driver_register` / `rt_i2c_device_register`**：将驱动/设备挂到 **I2C 总线模型**（见第 7 节）。

---

## 4. 核心路径（`dev_i2c_core.c`）

### 4.1 `rt_i2c_bus_device_register`

1. **`rt_mutex_init(&bus->lock, ...)`**  
2. **`timeout == 0`** 时设为 **`RT_TICK_PER_SECOND`**  
3. **`rt_i2c_bus_device_device_init(bus, bus_name)`**（注册 **`rt_device`**）  
4. **`RT_USING_DM`** 且成功时调用 **`i2c_bus_scan_clients(bus)`**（扫描子节点并 **`rt_i2c_device_register`**）

### 4.2 `rt_i2c_bus_device_find`

**`rt_device_find`** 后校验 **`type == RT_Device_Class_I2CBUS`**，返回 **`(struct rt_i2c_bus_device *)dev->user_data`**。

### 4.3 `rt_i2c_transfer`

若 **`ops->master_xfer`** 存在：**`rt_mutex_take(..., RT_WAITING_FOREVER)`** → **`master_xfer`** → **`rt_mutex_release`**；返回值直接来自驱动（通常为 **成功传输的 `msg` 条数** 或错误码转 **`rt_ssize_t`**）。无 **`master_xfer`** 返回 **`-RT_EINVAL`**。

### 4.4 `rt_i2c_control`

仅当 **`ops->i2c_bus_control`** 非空时转发；否则 **`-RT_EINVAL`**。

### 4.5 `rt_i2c_master_send` / `rt_i2c_master_recv`

组装单条 **`rt_i2c_msg`** 调 **`rt_i2c_transfer`**；若返回 **1** 则表示 1 条消息完成，函数返回 **`count`**，否则返回 **`ret`**（错误码或实际值）。

---

## 5. 设备层（`dev_i2c_dev.c`）

### 5.1 `read` / `write`

**`pos`**：**`addr = pos & 0xffff`，`flags = (pos >> 16) & 0xffff`**，再调用 **`rt_i2c_master_recv`/`send`**。适合 **VFS/简单封装**；组合写+读建议仍用 **`rt_i2c_transfer`** 或 **`RT_I2C_DEV_CTRL_RW`**。

### 5.2 `i2c_bus_device_control`

| `cmd` | 行为 |
|-------|------|
| **`RT_I2C_DEV_CTRL_10BIT`** | **`bus->flags \|= RT_I2C_ADDR_10BIT`** |
| **`RT_I2C_DEV_CTRL_TIMEOUT`** | 写 **`bus->timeout`** |
| **`RT_I2C_DEV_CTRL_RW`** | **`rt_i2c_transfer`**；**`ret < 0`** 时返回 **`-RT_EIO`**（注意 **`ret` 为 ssize_t** 时的分支） |
| **其它** | **`rt_i2c_control(bus, cmd, args)`** |

### 5.3 `rt_i2c_bus_device_device_init`

**`device->user_data = bus`**，类型 **`RT_Device_Class_I2CBUS`**，**`RT_DEVICE_FLAG_RDWR`**，挂 **`read`/`write`/`control`**（或 **`rt_device_ops`**）。

---

## 6. GPIO 位bang（`dev_i2c_bit_ops.c` + `dev_i2c_bit_ops.h`）

### 6.1 `struct rt_i2c_bit_ops`

**`set_sda`/`set_scl`、`get_sda`/`get_scl`（可空）、`udelay`、`delay_us`、`timeout`（tick）、`pin_init`、`i2c_pin_init_flag`**。首次 **`i2c_bit_xfer`** 前若 **`pin_init`** 非空则调用一次。

### 6.2 时序要点

- **`SCL_H`**：释放 SCL 后若提供 **`get_scl`**，则轮询直到高或 **`timeout`** 超时（**`-RT_ETIMEOUT`**）。
- **START / RESTART / STOP**：经典 GPIO 波形 + **`i2c_delay`/`i2c_delay2`**（**`udelay((delay_us+1)>>1)`** 与 **`udelay(delay_us)`**）。
- **写字节 / 读字节 / ACK**：与 **`RT_I2C_IGNORE_NACK`、`RT_I2C_NO_READ_ACK`、`RT_I2C_NO_START`、`RT_I2C_NO_STOP`** 协同。
- **10 位地址**：先发 **`0xf0 | ((addr>>7)&0x06)`**，再发低字节；读前 **Repeated Start** 并改 **`addr1 |= 1`**。

### 6.3 `rt_i2c_bit_add_bus`

**`bus->ops = &i2c_bit_bus_ops`**（仅 **`master_xfer`**），再 **`rt_i2c_bus_device_register`**。

---

## 7. 软件 I2C 实例（`dev_soft_i2c.c`）

- 编译期 **`#error`**：开启 **`RT_USING_SOFT_I2C`** 但未定义任一 **`RT_USING_SOFT_I2Cx`** 时直接报错，避免空表。
- **`struct rt_soft_i2c`**：内嵌 **`rt_i2c_bus_device` + `rt_i2c_bit_ops`**，**`ops.data`** 指向 **`soft_i2c_config`**（引脚号、总线名、延时等）。
- **引脚**：**`PIN_MODE_OUTPUT_OD`**，初始拉高 SCL/SDA。
- **`udelay`**：**`rt_hw_us_delay`**（需 BSP 或 DM 下有效实现）。
- **`rt_soft_i2c_init`**：**`INIT_PREV_EXPORT`**，对 **`i2c_cfg[]`** 中每条总线 **`rt_i2c_bit_add_bus`**，并调用 **`i2c_bus_unlock`**（最多 9 个 SCL 脉冲尝试释放总线卡死）。

---

## 8. DM 总线模型（`dev_i2c_bus.c`）

- 静态 **`struct rt_bus i2c_bus`**：**`name = "i2c"`**，**`match`/`probe`/`remove`/`shutdown`**。
- **`i2c_bus_init`**：**`INIT_CORE_EXPORT`** → **`rt_bus_register(&i2c_bus)`**。
- **`i2c_match`**：优先 **`driver->ids`** 与 **`client->name`** 字符串匹配；否则 OFW 下 **`rt_ofw_node_match`**。
- **`i2c_probe`**：要求 **`client->bus`** 已设置，调用 **`driver->probe(client)`**。
- **`i2c_bus_scan_clients`**（**`RT_USING_OFW`**）：遍历总线 **`ofw_node`** 下可用子节点，读 **`reg`** 为 **`client_addr`**，**`rt_calloc`** **`rt_i2c_client`** 后 **`rt_i2c_device_register`**；支持 **i2c-mux** 下一层子节点再取 **`compatible`** 节点。

---

## 9. 设备树时序（`dev_i2c_dm.c`）

**`RT_USING_OFW`** 下实现 **`i2c_timings_ofw_parse`**：从节点读取 **`clock-frequency`**、**`i2c-scl-rising-time-ns`** 等属性填入 **`struct i2c_timings`**；缺省值随 **`bus_freq_hz`** 分档（Standard/Fast 等）。无 OFW 时 **`dev_i2c_dm.h`** 内联桩返回 **`RT_EOK`**。

**`dev_i2c_dm.h`** 另定义 **I2C 模式频率上限常量**（Standard 100 kHz、Fast 400 kHz 等）供驱动参考。

---

## 10. BSP 驱动对接要点

1. 填充 **`struct rt_i2c_bus_device`**：**`ops->master_xfer`** 必须实现；需要 **`ioctl` 类`** 能力时实现 **`i2c_bus_control`**（如时钟、DMA、复用模式），并与 **`dev_i2c.h`** 中 **`RT_I2C_DEV_CTRL_*`** 及私有命令对齐。
2. **`rt_i2c_bus_device_register(bus, "i2c0")`**；应用通过 **`rt_i2c_bus_device_find`** 或 **`rt_device_find`** 获取总线。
3. **多消息传输**：同一 **`rt_i2c_transfer`** 内用 **`RT_I2C_NO_STOP`/`NO_START`** 拼 **写寄存器地址 + 读数据** 等原子序列。
4. 使用 **位bang** 时：实现 **`rt_i2c_bit_ops`**，**`bus->priv = &ops`**，调用 **`rt_i2c_bit_add_bus`**。

---

## 11. 小结与注意点

| 项目 | 说明 |
|------|------|
| 数据面主入口 | **`rt_i2c_transfer`**（带总线锁） |
| 设备文件语义 | **`read`/`write`** 用 **`pos`** 打包 **addr + flags** |
| 可选组件 | **位bang**、**软 I2C**、**DM 总线 + OFW 扫描/时序** |
| 头文件包含 | **`dev_i2c_bit_ops.h`** 依赖 **`RT_USING_I2C_BITOPS`**；**`dev_i2c_dm.h`** 依赖 **`RT_USING_DM`** |

实现细节：**`i2c_waitack`** 返回类型为 **`rt_bool_t`**，但存在 **`return -RT_ETIMEOUT`** 路径，与类型名略不一致（以整型语义使用）。宏 **`RT_I2C_DRIVER_EXPORT`** 中拼写为 **`BUILIN`**，与工程其它 **`BUILTIN`** 导出宏并存时需留意一致性。

阅读顺序：**`dev_i2c.h`**（消息与 ops）→ **`dev_i2c_core.c` / `dev_i2c_dev.c`** → **`dev_i2c_bit_ops.c`**（GPIO 时序）→ **`dev_soft_i2c.c`**（配置模板）→ DM 下 **`dev_i2c_bus.c` / `dev_i2c_dm.c`**。
