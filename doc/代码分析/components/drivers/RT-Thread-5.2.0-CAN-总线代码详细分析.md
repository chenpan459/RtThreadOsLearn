# RT-Thread 5.2.0 CAN 总线设备框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/can` 目录下的 **CAN 通用设备框架**（`dev_can.c`）及头文件 **`components/drivers/include/drivers/dev_can.h`** 中的数据结构与命令约定。具体控制器（bxCAN、FLEXCAN 等）由 BSP 实现 **`struct rt_can_ops`**，在中断里调用 **`rt_hw_can_isr()`** 与框架交互。

涉及文件：

- 框架实现：`rt-thread-5.2.0/components/drivers/can/dev_can.c`
- 对外接口与类型：`rt-thread-5.2.0/components/drivers/include/drivers/dev_can.h`（`rtdevice.h` 在 `RT_USING_CAN` 下包含）
- 说明文档（偏驱动作者视角）：`readme-zh.txt`
- 构建：`Kconfig`、`SConscript`

---

## 1. 模块定位与分层

```text
应用 / Finsh 示例
    ↓
rt_device：open/read/write/control（中断 RX/TX 模式）
    ↓
dev_can.c：软件 RX/TX 队列、完成量、可选 HDR 过滤表、周期状态定时器
    ↓
rt_can_ops：configure / control / sendmsg / recvmsg
    ↓
硬件：CAN 控制器 + NVIC 中断
```

**设计要点**：当前框架路径以 **中断驱动** 为主（`open` 时分配 `can_rx`/`can_tx` 并 `SET_INT`）。`readme-zh.txt` 说明历史版本 **未实现轮询模式**；若 BSP 仅实现中断路径，则 `open` 时必须带 **`RT_DEVICE_FLAG_INT_RX` / `INT_TX`** 才会挂载内部 FIFO，否则 `read`/`write` 返回 0。

---

## 2. Kconfig 与编译

| 选项 | 含义 |
|------|------|
| `RT_USING_CAN` | 总开关；`SConscript` 在开启时编译 `Glob('*.c')`，即 **`dev_can.c`** |
| `RT_CAN_USING_HDR` | 硬件过滤与 **按 hdr 分桶** 的接收路径；`dev_can.h` 中 **`rt_can_filter_item`** 等宏会展开出 `ind/args` 字段 |
| `RT_CAN_USING_CANFD` | **`struct rt_can_msg`** 扩展为 64 字节数据区，并增加 **FD/BRS**、**位时序** 等 `can_configure` 字段 |

默认宏（可在 `rtconfig.h` 或编译选项覆盖）：**`RT_CANMSG_BOX_SZ`**（接收邮箱槽位数相关）、**`RT_CANSND_BOX_NUM`**（发送邮箱数缺省 1）。

---

## 3. 关键数据结构（`dev_can.h`）

### 3.1 `struct rt_can_device`

- **`parent`**：`rt_device`，类型为 **`RT_Device_Class_CAN`**。
- **`ops`**：BSP 实现的 **`rt_can_ops`**。
- **`config`**：**`struct can_configure`**（波特率、`msgboxsz`、`sndboxnumber`、模式、`privmode`、`ticks`；HDR 下 **`maxhdr`**；CANFD 下 **`baud_rate_fd`、`enable_canfd`、`use_bit_timing`** 及 **`rt_can_bit_timing`** 等）。
- **`status`**：**`struct rt_can_status`**（收发包计数、各类总线错误计数、`errcode` 等），由 **`RT_CAN_CMD_GET_STATUS`** 经驱动填充。
- **`timer` / `timerinitflag`**：周期定时器 **`cantimeout`**，用于轮询状态并回调 **`status_indicate`** / **`bus_hook`**。
- **`status_indicate`**：应用通过 **`RT_CAN_CMD_SET_STATUS_IND`** 设置。
- **`lock`**：互斥锁，**`open`/`close`** 等路径 **`CAN_LOCK`**。
- **`can_rx` / `can_tx`**：分别指向 **`struct rt_can_rx_fifo`**、**`struct rt_can_tx_fifo`**（在 **`rt_can_open`** 中 `malloc`）。
- **`hdr`**（`RT_CAN_USING_HDR`）：长度为 **`maxhdr`** 的 **`struct rt_can_hdr`** 数组，在 **`open`** 中分配。

### 3.2 `struct rt_can_ops`

| 成员 | 职责 |
|------|------|
| `configure` | 按 **`can_configure`** 初始化控制器（波特率、模式等） |
| `control` | 处理 **`RT_DEVICE_CTRL_SET_INT/CLR_INT`**、**`RT_CAN_CMD_*`** 等；框架未识别的 `cmd` 也会落到此处 |
| `sendmsg` | 将 **`struct rt_can_msg`** 写入指定发送 **`boxno`**；成功返回 **`RT_EOK`**（注意类型为 **`rt_ssize_t`**，与 readme 中历史 `int` 描述略有出入） |
| `recvmsg` | 从硬件邮箱 **`no`** 读出到 **`rt_can_msg`**；失败返回 **`-1`** |

### 3.3 `struct rt_can_msg`

位域：**`id`(29)、`ide`、`rtr`、`len`、`priv`（私有发送模式下的邮箱索引）、`hdr_index`（接收匹配的过滤组索引）** 等。  
**`RT_CAN_USING_CANFD`** 下增加 **`fd_frame`、`brs`**，**`data[64]`**；否则 **`data[8]`**。

### 3.4 软件接收 FIFO：`struct rt_can_rx_fifo`

- **`buffer`**： **`rt_can_msg_list`** 数组，长度 **`config.msgboxsz`**。
- **`freelist` / `uselist`**：空闲链表与已填充待读链表。
- **`freenumbers`**：空闲个数。

每个 **`rt_can_msg_list`** 内含 **`struct rt_can_msg data`**；HDR 模式下另有 **`hdrlist`** 与 **`owner`** 指向某个 **`rt_can_hdr`**。

### 3.5 软件发送 FIFO：`struct rt_can_tx_fifo`

- **`buffer[sndboxnumber]`**：每个元素为 **`rt_can_sndbxinx_list`**（链表节点 + **`completion`** + **`result`**）。
- **`sem`**：初值为 **`sndboxnumber`**，**`_can_int_tx`** 中 **`rt_sem_take`** 与发送槽占用配对。
- **`freelist`**：空闲发送槽链表。

### 3.6 过滤与 HDR（`RT_CAN_USING_HDR`）

- **`rt_can_filter_item`**：`id/ide/rtr/mode/mask`、`hdr_bank`（硬件过滤组号）、`rxfifo`，以及可选 **`ind(dev, args, hdr, size)`**。
- **`rt_can_filter_config`**：`count`、`actived`、**`items`** 指针。
- 框架在 **`RT_CAN_CMD_SET_FILTER`** 中根据 **`actived`** 同步 **`can->hdr[i]`** 的 **`filter`** 与 **`connected`** 标志，并维护每个 hdr 上的 **`list`**（挂接收 **`rt_can_msg_list`**）。

### 3.7 框架层控制命令宏

除 **`RT_DEVICE_CTRL_CONFIG`** 外，常用 **`RT_CAN_CMD_*`**：

`SET_FILTER`、`SET_BAUD`、`SET_MODE`、`SET_PRIV`、`GET_STATUS`、`SET_STATUS_IND`、`SET_BUS_HOOK`（`RT_CAN_USING_BUS_HOOK`）、**`SET_CANFD`/`SET_BAUD_FD`/`SET_BITTIMING`**、`START`。

**`RT_DEVICE_CAN_INT_ERR`**（`0x1000`）：与 **`RT_DEVICE_FLAG_INT_RX/TX`** 一起在 **`open`** 里通过 **`control(SET_INT, ...)`** 打开错误类中断。

---

## 4. 设备生命周期（`dev_can.c`）

### 4.1 `rt_hw_can_register()`

1. 设置 **`device->type = RT_Device_Class_CAN`**，清空 **`rx_indicate`/`tx_complete`**（由应用按需设置）。
2. **`rt_mutex_init(&can->lock)`**。
3. 绑定 **`init/open/close/read/write/control`**（或 **`can_device_ops`**）。
4. **`can->ops = ops`**，清零 **`status`**，**`status_indicate`**。
5. **`rt_timer_init(..., cantimeout, can, config.ticks, RT_TIMER_FLAG_PERIODIC)`** — 定时周期由 **`config.ticks`** 决定；**`rt_timer_start` 在首次 `open` 时执行**。
6. **`rt_device_register(..., RT_DEVICE_FLAG_RDWR)`**。

注意：**注册阶段不分配 `can_rx/can_tx/hdr`**，均在 **`rt_can_open`** 中按 **`oflag`** 与 Kconfig 分配。

### 4.2 `rt_can_init()`（`device->init`）

调用 **`ops->configure(can, &can->config)`**；若无 **`configure`** 则 **`-RT_ENOSYS`**。

### 4.3 `rt_can_open()`

在 **`CAN_LOCK`** 下：

1. **`RT_DEVICE_FLAG_INT_RX`** 且 **`can_rx == NULL`**：分配 **`rt_can_rx_fifo` + msgboxsz 个 rt_can_msg_list`**，初始化 **`freelist/uselist`**，**`SET_INT(RX)`**。
2. **`RT_DEVICE_FLAG_INT_TX`** 且 **`can_tx == NULL`**：分配 **`rt_can_tx_fifo` + sndboxnumber 个 sndbxinx_list`**，每槽 **`completion_init`**、**`result=OK`**，**`sem` 初值 = sndboxnumber**，**`SET_INT(TX)`**。
3. **`SET_INT(RT_DEVICE_CAN_INT_ERR)`**。
4. **`RT_CAN_USING_HDR`**：分配 **`hdr[maxhdr]`**，每元素初始化 **`list`**。
5. 若 **`timerinitflag` 为 0**：置 1 并 **`rt_timer_start(&can->timer)`**。

### 4.4 `rt_can_close()`

- **`ref_count > 1`**：仅返回 **`RT_EOK`**（多引用时不释放资源）。
- 停止定时器，清除 **`status_indicate`**，释放 **`hdr`**。
- 关闭 RX/TX 中断并 **`rt_free`** 对应 FIFO；**`rt_sem_detach`** 发送侧信号量。
- **`RT_CAN_CMD_START, RT_FALSE`** 交给驱动停总线（语义由 BSP 定义）。

---

## 5. 读路径：`_can_int_rx()`

由 **`rt_can_read`** 在已打开 **INT_RX** 时调用。

- 按 **`size`** 以 **`sizeof(struct rt_can_msg)`** 为步长从软件 FIFO 取帧。
- **HDR**：若 **`data->hdr_index >= 0`** 且对应 **`hdr[hdr].list`** 非空，从 **hdr 专用链表** 取 **`rt_can_msg_list`**；若 **`hdr_index == -1`**，从 **`rx_fifo->uselist`** 取。
- 拷贝 **`listmsg->data`** 到用户 **`data`**，再把 **`listmsg`** 插回 **`freelist`**，**`freenumbers++`**。
- 无数据时退出循环，返回已读字节数。

应用读前通常依赖 **`rx_indicate`** 或 HDR 的 **`ind`** 唤醒线程。

---

## 6. 写路径：`_can_int_tx()` 与 `_can_int_tx_priv()`

### 6.1 普通模式（`privmode == 0`）：`_can_int_tx`

1. **`rt_sem_take(tx_fifo->sem)`** 占用一个逻辑发送配额。
2. 从 **`freelist`** 取一个 **`tx_tosnd`**，计算其 **`boxno`**（在 **`buffer[]` 中的下标**）。
3. **`completion_init`**，**`result = WAIT`**，调用 **`sendmsg(can, data, no)`**；失败则归还槽并 **`goto err_ret`**（丢包计数）。
4. **`completion_wait`** 等待 ISR 置 **`TX_DONE/TX_FAIL`**。
5. 成功则 **`sndpkg++`**，并继续下一帧；失败 **`dropedsndpkg++`**。

### 6.2 私有邮箱模式（`privmode != 0`）：`_can_int_tx_priv`

使用用户 **`rt_can_msg.priv`** 作为 **`boxno`**（须 **`< sndboxnumber`**）。每槽独立 **`completion`**，**不经过 `tx_fifo->sem` 的通用配额逻辑**（与 `SET_PRIV` 动态切换时 `rt_can_control` 中对链表/信号量的修补配合）。

---

## 7. 控制接口：`rt_can_control()` 摘要

| `cmd` | 行为 |
|-------|------|
| `RT_DEVICE_CTRL_SUSPEND/RESUME` | 置位/清除 **`RT_DEVICE_FLAG_SUSPENDED`** |
| `RT_DEVICE_CTRL_CONFIG` | **`ops->configure(can, args)`** |
| `RT_CAN_CMD_SET_PRIV` | 调 **`ops->control`** 后，根据新旧 **`privmode`** 调整 **`tx_fifo`** 中各槽链表与 **`sem`**（源码见 `dev_can.c`） |
| `RT_CAN_CMD_SET_STATUS_IND` | 记录 **`status_indicate.ind/args`** |
| `RT_CAN_CMD_SET_FILTER` | HDR 下先 **`ops->control`**，再按 **`actived`** 更新 **`can->hdr[]`** |
| `RT_CAN_CMD_SET_BUS_HOOK` | 记录 **`bus_hook`** |
| `default` | 透传 **`ops->control(can, cmd, args)`** |

---

## 8. 中断入口：`rt_hw_can_isr(struct rt_can_device *can, int event)`

**事件编码**：**低 8 位**（`event & 0xff`）为 **`RT_CAN_EVENT_*`**；**高字节区域**（代码中用 **`event >> 8`**）为 **硬件邮箱/通道号 `no`**，供 **`recvmsg(can, &tmpmsg, no)`** 与 **`tx_fifo->buffer[no]`** 索引。

### 8.1 `RT_CAN_EVENT_RXOF_IND` / `RT_CAN_EVENT_RX_IND`

- **`RXOF_IND`**：无 **`break`**，**落入** **`RX_IND`** 分支（C switch 贯穿），先 **`dropedrcvpkg++`** 再处理接收。
- **`RX_IND`**：调用 **`recvmsg`**；返回 **`-1`** 则 **`break`**。
- 若 **`freelist` 非空**：取空闲 **`listmsg`**，填入数据后加入 **`uselist`**；**HDR** 下若 **`hdr[hdr_index].connected`**，再挂到对应 **`hdr[].list`** 并维护 **`msgs`**。
- 若 **`freelist` 空** 且 **`uselist` 非空**：丢弃最旧一帧（**`dropedrcvpkg++`**），复用其 **`listmsg`** 填新数据（环形覆盖语义）。
- 回调：**HDR 且 `filter.ind` 非空** 时调 **`ind(dev, args, hdr, rx_length)`**；否则 **`parent.rx_indicate(dev, rx_length)`**，其中 **`rx_length = rt_list_len(uselist) * sizeof(rt_can_msg)`**。

### 8.2 `RT_CAN_EVENT_TX_DONE` / `RT_CAN_EVENT_TX_FAIL`

根据 **`no = event >> 8`** 设置 **`tx_fifo->buffer[no].result`** 为 **OK** 或 **ERR**，**`rt_completion_done`** 唤醒 **`_can_int_tx`** 中的等待。

---

## 9. 周期定时器：`cantimeout()`

每次触发：

1. **`rt_device_control(..., RT_CAN_CMD_GET_STATUS, &can->status)`** 刷新软件可见状态。
2. 若注册了 **`status_indicate.ind`**，则调用之。
3. **`RT_CAN_USING_BUS_HOOK`**：调用 **`bus_hook(can)`**。
4. 若 **`timerinitflag == 1`**，将其置为 **`0xFF`**（与 **`open`/`close`** 配合，避免重复逻辑；具体数值用于区分“未启动/已启动”等内部状态）。

---

## 10. Finsh：`canstat`

在 **`RT_USING_FINSH`** 下导出 **`canstat`**（`cmd_canstat`）：根据设备名 **`rt_device_find`**，再 **`RT_CAN_CMD_GET_STATUS`** 打印错误计数与 **`errcode`** 对应文字。

---

## 11. BSP 驱动实现清单（对照 `readme-zh.txt`）

1. 静态初始化 **`struct rt_can_device`** 与 **`can_configure`**（`msgboxsz`、`sndboxnumber`、`ticks` 等）。
2. 实现 **`rt_can_ops`**，在 **`control`** 中至少支持：**`RT_DEVICE_CTRL_SET_INT/CLR_INT`**（`arg` 为 **`RT_DEVICE_FLAG_INT_RX/TX`** 或 **`RT_DEVICE_CAN_INT_ERR`**）、框架下发的 **`RT_CAN_CMD_*`**（按芯片能力选做）。
3. **接收中断**：从硬件 FIFO/邮箱取帧后调用 **`rt_hw_can_isr(can, RT_CAN_EVENT_RX_IND | (no << 8))`**；溢出用 **`RT_CAN_EVENT_RXOF_IND`**。
4. **发送完成/失败中断**：**`rt_hw_can_isr(can, RT_CAN_EVENT_TX_DONE | (no << 8))`** 或 **`TX_FAIL`**。
5. **`recvmsg`**：无数据或错误时返回 **`-1`**，否则返回非负（源码中返回值赋给 **`ch`**，与 **`-1`** 比较）。
6. **`sendmsg`**：启动发送，成功返回 **`RT_EOK`**。

`readme-zh.txt` 中列举的旧 BSP 路径（如 **`bsp/stm32f10x`**）仅作历史参考，实际仓库请以当前芯片的 **`drv_can.c`** 为准。

---

## 12. 小结

| 项目 | 说明 |
|------|------|
| 目录职责 | CAN **字符设备框架**：软 FIFO、发送完成量、HDR 过滤扩展、状态定时器、Finsh 统计命令 |
| 数据面 | **`read`/`write` 以 `struct rt_can_msg` 为单位**；长度应为 **`sizeof(rt_can_msg)` 的整数倍** |
| 中断契约 | **`rt_hw_can_isr`** 低 8 位事件类型，高位传递 **mailbox 索引** |
| 扩展 | **CANFD**、**HDR**、**BUS_HOOK** 均由 Kconfig 与头文件条件编译包裹 |

按 **`dev_can.h`（类型与命令）→ `dev_can.c`（open/ISR/read/write）→ BSP `drv_can.c`** 的顺序阅读，可快速建立从总线中断到应用 **`rt_device_read`** 的完整链路。
