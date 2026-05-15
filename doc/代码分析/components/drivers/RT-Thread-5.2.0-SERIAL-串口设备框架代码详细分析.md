# RT-Thread 5.2.0 Serial（串口字符设备框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/serial` 目录实现的 **UART 串口设备框架**：在 **`RT_USING_SERIAL`** 下通过 **`RT_USING_SERIAL_V2`** 在 **V1（`dev_serial.h` + `dev_serial.c`）** 与 **V2（`dev_serial_v2.h` + `dev_serial_v2.c`）** 之间二选一编译；可选 **DMA（`RT_SERIAL_USING_DMA`）**、**POSIX 文件层（`RT_USING_POSIX_STDIO`）**、**Serial Bypass（`RT_USING_SERIAL_BYPASS`，仅 V1 头文件集成）**、**设备树命名辅助（`serial_dm.c`，`RT_USING_DM`）**、**Smart 终端 TTY（`serial_tty.c`，`RT_USING_SMART`）**。

**`rtdevice.h`** 在串口子系统上的包含关系如下（**V2 时不包含 `serial_bypass.h`**，因 **`struct rt_serial_device`** 在 V2 中无 **`bypass`** 成员）：

```150:159:rt-thread-5.2.0/components/drivers/include/rtdevice.h
#ifdef RT_USING_SERIAL
#ifdef RT_USING_SERIAL_V2
#include "drivers/dev_serial_v2.h"
#else
#include "drivers/dev_serial.h"
#ifdef RT_USING_SERIAL_BYPASS
#include "drivers/serial_bypass.h"
#endif /* RT_USING_SERIAL_BYPASS */
#endif
#endif /* RT_USING_SERIAL */
```

---

## 1. 目录与编译（`SConscript`）

| 文件 | 条件 | 作用 |
|------|------|------|
| **`dev_serial.c`** | **`RT_USING_SERIAL` 且未选 V2** | **V1** 设备层：轮询/中断/DMA RX-TX、**`rt_hw_serial_isr`**、注册 |
| **`dev_serial_v2.c`** | **`RT_USING_SERIAL_V2`** | **V2** 设备层：**环形缓冲 + blocking/non-blocking**、**`transmit` 统一发送** |
| **`bypass.c`** | **`RT_USING_SERIAL_BYPASS`** | **旁路链**：上/下层钩子、**ringbuffer 管道**、工作队列 |
| **`serial_dm.c`** | **`RT_USING_DM`** | **`serial_dev_set_name`**（OFW **`serial`/`uart` alias**）、**`serial_cfg_from_args`**（earlycon/bootargs 风格串口参数） |
| **`serial_tty.c`** | **`RT_USING_SMART`** | **LWP TTY**：与 **`terminal`** 绑定、**`rt_hw_serial_register_tty`** |
| **`Kconfig`** | — | 版本选择、**`RT_SERIAL_RB_BUFSZ`**（仅 V1）、**`RT_SERIAL_USING_DMA`**、**`RT_USING_SERIAL_BYPASS`** |

**`SConscript`** 逻辑摘要：

- 未 **`RT_USING_SERIAL`**：不加入本组。
- **`RT_USING_SMART`**：追加 **`serial_tty.c`**。
- **`RT_USING_SERIAL_V2`**：**`dev_serial_v2.c`**，否则 **`dev_serial.c`**。
- **`RT_USING_SERIAL_BYPASS`**：**`bypass.c`**（与 V1 数据结构配套）。
- **`RT_USING_DM`**：**`serial_dm.c`**。

---

## 2. Kconfig 要点

| 选项 | 含义 |
|------|------|
| **`RT_USING_SERIAL`** | 总开关；**`select RT_USING_DEVICE_IPC`、 `RT_USING_DEVICE`** |
| **`RT_USING_SERIAL_V1` / `RT_USING_SERIAL_V2`** | **choice**，默认 **V1** |
| **`RT_SERIAL_USING_DMA`** | 允许 **DMA 收发路径**（与 **`dev_serial.c`** 中 **`#ifdef RT_SERIAL_USING_DMA`** 对应） |
| **`RT_SERIAL_RB_BUFSZ`** | **仅 `depends on !RT_USING_SERIAL_V2`**：V1 默认软件 RX 环大小 |
| **`RT_USING_SERIAL_BYPASS`** | 编译 **旁路**（默认 **n**） |

---

## 3. 数据结构与 HAL 操作表

### 3.1 共性

- **`struct rt_serial_device`**：**内嵌 `struct rt_device parent`**，**`const struct rt_uart_ops *ops`**，**`struct serial_configure config`**，**`void *serial_rx` / `serial_tx`**（具体类型由 **轮询 / 中断 / DMA** 决定）。
- **`rt_hw_serial_register`**：登记为 **`RT_Device_Class_Char`**，挂 **`init/open/close/read/write/control`**（或 **`rt_device_ops`**），可选 **`fops`**（POSIX）、**V1 下 Smart 时额外 `rt_hw_serial_register_tty`**。

### 3.2 V1 特有（`dev_serial.h`）

- **`serial_configure.bufsz`**：RX 软件 FIFO 长度（**`RT_SERIAL_RB_BUFSZ`**）。
- **`struct rt_serial_rx_fifo`**：**`put_index`/`get_index`/`is_full` + 柔性数组缓冲**。
- **`struct rt_serial_tx_dma`**：**`rt_data_queue`** 排队多段 DMA 发送。
- **`struct rt_uart_ops`**：**`configure`、`control`、`putc`、`getc`、`dma_transmit`**。
- **`spinlock`**、**`rx_notify`**；**`#ifdef RT_USING_SERIAL_BYPASS`** 时 **`struct rt_serial_bypass *bypass`**。

### 3.3 V2 特有（`dev_serial_v2.h`）

- **`serial_configure`**：**`rx_bufsz`、`tx_bufsz` 分离**（各 16bit），默认 **`RT_SERIAL_RX_MINBUFSZ` / `TX_MINBUFSZ`**（64）。
- **`rt_serial_rx_fifo` / `rt_serial_tx_fifo`**：内嵌 **`struct rt_ringbuffer rb`**，尾部 **`uint8_t buffer[]`**；RX 带 **`rx_cpt`/`rx_cpt_index`** 支持 **blocking 读完成量**。
- **`rt_uart_ops`**：将 V1 的 **`dma_transmit`** 泛化为 **`transmit(serial, buf, size, tx_flag)`**（**中断/DMA/阻塞标志**由上层与 HAL 约定）。
- **打开标志**：**`RT_DEVICE_FLAG_RX_BLOCKING` / `RX_NON_BLOCKING`、`TX_BLOCKING` / `TX_NON_BLOCKING`** 等与 **`open_flag` 高 12 位** 协同（见 **`rt_serial_open`** 中 **`15 << 12`** 的“已打开”判断）。

---

## 4. `dev_serial.c`（V1）核心流程

### 4.1 `rt_serial_open`

- 校验 **`oflag`** 与设备 **`flag`** 是否支持 **DMA_RX/TX、INT_RX/TX**。
- **`RT_DEVICE_FLAG_STREAM`** 与 **`dev->open_flag`** 合并（**`\n` 自动补全**等流模式在 write 路径处理）。
- **首次打开**时按模式分配：
  - **INT_RX**：**`malloc(sizeof(rx_fifo)+bufsz)`**，**`RT_DEVICE_CTRL_SET_INT`**。
  - **DMA_RX**：**`bufsz==0`** 为纯 **`rt_serial_rx_dma`**；否则 **FIFO + `RT_DEVICE_CTRL_CONFIG` DMA_RX**。
  - **INT_TX**：**`rt_serial_tx_fifo` + completion + SET_INT TX**。
  - **DMA_TX**：**`rt_data_queue_init` + CONFIG DMA_TX**。
- **`RT_USING_PINCTRL`**：**`rt_pin_ctrl_confs_apply_by_name`** 应用管脚。

### 4.2 `rt_hw_serial_isr`

- **`RT_SERIAL_EVENT_RX_IND`**：**循环 `ops->getc`** 写入 RX FIFO；**满时顶掉最旧数据**并调用 **`_serial_check_buffer_size()`**（调试统计）。
  - **`RT_USING_SERIAL_BYPASS`**：**upper 链**在 **入 FIFO 前**逐层调用 **`bypass(serial, ch, data)`**，返回 0 表示 **消费掉该字节**；**lower 链** 通过 **`rt_workqueue_dowork`** 在 **线程上下文** 从 FIFO 取字节再处理。
- **`RT_SERIAL_EVENT_TX_DONE`**：**`completion_done`**（中断发送完成）。
- **DMA 事件**：**TX_DMADONE** 从 **`data_queue` pop** 并可能 **启动下一段 `dma_transmit`**；**RX_DMADONE** 的 **`event>>8`** 为长度，**`bufsz==0`** 时直接 **`rx_indicate(length)`**，否则 **更新 put 索引**再通知。

### 4.3 `rt_hw_serial_register`

- **`rt_spin_lock_init(&serial->spinlock)`**。
- 注册后：**`#ifdef RT_USING_POSIX_STDIO` → `device->fops = &_serial_fops`**。
- **`#if defined(RT_USING_SMART)` → `rt_hw_serial_register_tty(serial)`**（**V2 的 `rt_hw_serial_register` 中无此调用**）。

### 4.4 POSIX `fops`（两版均存在，细节略异）

- **`open`**：V1 对读侧附加 **`RT_DEVICE_FLAG_INT_RX`**；V2 **不**在 fops 里加 **`INT_RX`**，依赖 **V2 `open_flag` 的 blocking 模型**。
- **`read`**：无数据时在 **`wait_queue`** 上睡眠；V1 使用 **`rt_wqueue_wait_interruptible`**，V2 使用 **`rt_wqueue_wait`**。
- **`poll`**：V1 用 **`spinlock` + 索引比较** 判断是否有数据；V2 用 **`rt_ringbuffer_data_len`**。

---

## 5. `dev_serial_v2.c`（V2）核心流程

### 5.1 `rt_serial_init`

- **断言 `ops->transmit != RT_NULL`**（V2 发送路径强依赖）。

### 5.2 `rt_serial_open`

- 若 **`open_flag` 已带 `(15<<12)`** 认为 **已打开过**，直接 **`RT_EOK`**（重复 open 不改变配置）。
- 默认 **RX：`NON_BLOCKING`**（除非显式 **`RT_SERIAL_RX_BLOCKING`**）；**TX：默认 `BLOCKING`**（除非 **`RT_SERIAL_TX_NON_BLOCKING`**）。
- **`rt_serial_rx_enable` / `rt_serial_tx_enable`**：内部按 **`rx_bufsz`/`tx_bufsz`** 分配 **带 `rt_ringbuffer` 的 FIFO** 或走 **纯轮询**，并 **`ops->control`** 打开 **DMA/中断** 等。

### 5.3 `rt_hw_serial_isr`

- **`RX_IND` 与 `RX_DMADONE` 合并分支**：DMA 时在 ISR 里 **`rt_serial_update_write_index`**，再算 **`rt_ringbuffer_data_len`**；**blocking RX** 且达到 **`rx_cpt_index`** 时 **`rt_completion_done`**。
- 调用 **`rx_indicate`**；若 **`rx_notify.notify` 非空** 再通知 **挂接设备**（与 V1 的 `rx_notify` 字段在头文件中的存在性一致，V1 `init` 会 **`memset` rx_notify**，V2 同样可使用）。
- **`TX_DONE`**：若 **TX ringbuffer 空** → **`tx_complete`**；**blocking** 则 **`completion_done(tx_cpt)`**，否则 **`activated = RT_FALSE`**；非空则再次 **`ops->transmit(..., open_flag & (BLOCKING|NON_BLOCKING))`**。
- **`TX_DMADONE`**：**`activated = RT_FALSE`**，**`tx_complete`**，blocking 完成量；非 blocking 则 **`update_read_index`** 并可能 **连续 `transmit` 下一段线性区**。

### 5.4 注册与 `read`/`write`

- 在 **`RT_USING_DEVICE_OPS`** 下使用 **`serial_ops`**，**`read`/`write` 分派到 `_serial_fifo_rx`、`_serial_poll_rx`、多种 TX 路径**。
- **若未定义 `RT_USING_DEVICE_OPS`**：**`device->read`/`write` 被设为 `RT_NULL`**（仅 **`control`/`open`/`close`/`init`**），实际产品应 **开启 `RT_USING_DEVICE_OPS`** 或 **仅通过 DFS `fops` 访问**。

---

## 6. `bypass.c`（旁路，依赖 V1 设备布局）

- **`rt_serial_bypass_init`**：**`malloc` `struct rt_serial_bypass`**，**`pipe = rt_ringbuffer_create(serial->config.bufsz)`**，互斥锁。
- **Upper**：在 **`rt_hw_serial_isr` RX 路径**（**ISR 上下文**）按 **level 升序**（**数值小优先**）调用；**`bypass` 返回 0** 表示字节 **不再进入 FIFO**。
- **Lower**：**工作队列 `lower_workq`** 执行 **`_lower_work`**：从 **硬件 RX FIFO** 取字节（**`_bypass_getchar_form_serial_fifo`**），再经 **lower 链**；若链尾仍 **未消费** 则 **`rt_bypass_putchar` 写入 pipe**（供上层取走）。
- **`rt_bypass_unregister`**：**保护级别 `> RT_BYPASS_PROTECT_LEVEL_1`（10）不可卸载**（注释：留给 MSH/TTY）。
- **`MSH_CMD_EXPORT(serial_bypass_list, ...)`**：列出 **控制台串口** 上的 upper/lower 链。

---

## 7. `serial_dm.c`（DM / earlycon 辅助）

- **`serial_dev_set_name`**：优先 **OFW `serial`/`uart` alias 数字**；否则 **原子递增 uid**；最终 **`rt_dm_dev_set_name(..., "uart%u", id)`**。
- **`INIT_PLATFORM_EXPORT(serial_dm_naming_framework_init)`**：在 OFW 下把 **uid 初值** 设为 **alias 最大 id + 1**，避免与 DT 别名冲突。
- **`serial_cfg_from_args`**：解析 **`115200n8r`** 形式字符串为 **`struct serial_configure`**（**波特率/校验/数据位/RTS 流控**）；可选 **OFW earlycon 魔数**分支。

---

## 8. `serial_tty.c`（Smart / LWP）

- 为 **`struct rt_serial_device`** 注册 **TTY 设备名**：**DM 模式下** 要求底层名以 **`uart`** 开头，TTY 名为 **`S` + 去掉 `uart` 前缀后的后缀**；非 DM 下为 **`S0`、`S1`…**。
- **`rt_hw_serial_register_tty`**：创建 **`terminal`/`lwp_tty`**、安装 **notify/work**、可与 **bypass** 协同（如 **`_serial_ty_bypass`**）。

**注意**：当前 **`rt_hw_serial_register_tty` 仅在 `dev_serial.c` 的 `rt_hw_serial_register` 末尾调用**；**纯 V2 + Smart** 场景需自行确认是否另有注册路径或补丁。

---

## 9. 驱动编写要点（HAL）

| 项目 | V1 | V2 |
|------|----|----|
| **收字节** | **`getc`**，ISR 里 **`rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_IND)`** | 仍通过 **`rt_hw_serial_isr`**；DMA 时带 **长度：`event \| (len << 8)`**（与 V1 约定一致） |
| **发** | **轮询 `putc`** 或 **INT/DMA + `dma_transmit`** | 优先 **`transmit(..., tx_flag)`** |
| **控制** | 响应 **`RT_DEVICE_CTRL_SET_INT/CLR_INT/CONFIG`** 等 | 由 **`rt_serial_rx_enable` 等** 间接 **`ops->control`**；close 时 **`RT_DEVICE_CTRL_CLOSE`** |

---

## 10. 小结

| 模块 | 职责 |
|------|------|
| **`dev_serial.c`** | 经典 **索引 FIFO + `dma_transmit`**，**Bypass**，**Smart TTY 挂钩** |
| **`dev_serial_v2.c`** | **双缓冲尺寸、ringbuffer、blocking 语义、`transmit` 统一** |
| **`bypass.c`** | **ISR 上层过滤 + 线程下层抽帧 + pipe** |
| **`serial_dm.c`** | **DT 友好命名与 earlycon 参数字符串** |
| **`serial_tty.c`** | **Smart 终端与串口桥接** |

选型建议：**新 BSP 若已统一 V2 驱动模型，用 `RT_USING_SERIAL_V2`**；需要 **Bypass / 与现网 V1 BSP 兼容** 时保持 **V1**，并注意 **`rtdevice.h` 仅在 V1 下暴露 `serial_bypass.h`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/serial/`。*
