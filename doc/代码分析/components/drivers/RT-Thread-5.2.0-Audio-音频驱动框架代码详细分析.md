# RT-Thread 5.2.0 Audio 音频驱动框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/audio` 目录实现的是**通用音频设备框架**（非具体 Codec/DAC 芯片驱动）。具体硬件由 BSP 实现 `struct rt_audio_ops`，通过 `rt_audio_register()` 挂接到本框架。

涉及文件：

- 框架实现：`dev_audio.c`
- 录音侧管道：`dev_audio_pipe.c`、本地头文件 `dev_audio_pipe.h`
- 对外 API 与数据结构：`rt-thread-5.2.0/components/drivers/include/drivers/dev_audio.h`（由 `rtdevice.h` 在启用音频时包含）
- 配置与构建：`Kconfig`、`SConscript`

---

## 1. 模块定位与分层

```text
应用 / 中间件（如播放器）
    ↓
rt_device（类型 Sound）：read / write / control
    ↓
本目录：回放队列 + 环形录音管道 + 控制命令分发
    ↓
rt_audio_ops：init / start / stop / transmit / getcaps / configure / buffer_info
    ↓
BSP：I2S、SAI、DMA、Codec 寄存器
```

- **框架层（`dev_audio.c`）**：管理 `RT_DEVICE_FLAG_RDONLY` / `WRONLY` 对应的 **录音管道** 与 **回放内存池 + 数据队列**，把应用 `write()` 的 PCM 数据整理成硬件 `transmit()` 所需的块，并在 DMA/中断完成时继续喂数。
- **管道层（`dev_audio_pipe.c`）**：基于 `rt_ringbuffer` 的 **Pipe 设备**，支持阻塞读、强制写（满则覆盖）、`PIPE_CTRL_GET_SPACE` 查询剩余空间；供录音数据从 ISR 写入、应用线程 `read()` 读出。
- **芯片层（不在本目录）**：实现 `rt_audio_ops`，在合适时机调用 **`rt_audio_tx_complete()`**、**`rt_audio_rx_done()`** 与框架交互。

---

## 2. Kconfig 与 SConscript

### 2.1 `RT_USING_AUDIO`

打开后暴露三项可配参数：

| 配置项 | 含义 | 默认 |
|--------|------|------|
| `RT_AUDIO_REPLAY_MP_BLOCK_SIZE` | 回放内存池单块字节数 | 4096 |
| `RT_AUDIO_REPLAY_MP_BLOCK_COUNT` | 回放内存池块数量 | 2 |
| `RT_AUDIO_RECORD_PIPE_SIZE` | 录音 Pipe 底层环形缓冲区大小 | 2048 |

### 2.2 `SConscript`

- 使用 `Glob('*.c')` 编译目录下所有 `.c`（即 `dev_audio.c` 与 `dev_audio_pipe.c`）。
- `depend = ['RT_USING_AUDIO']`。
- `CPPPATH` 设为当前 `audio` 目录，因此 **`#include "dev_audio_pipe.h"`** 可直接解析。

---

## 3. 对外数据结构（`dev_audio.h`）

### 3.1 控制命令

基于 `RT_DEVICE_CTRL_BASE(Sound)` 偏移：

- `AUDIO_CTL_GETCAPS`：查询能力（采样率掩码、声道、Mixer 等）。
- `AUDIO_CTL_CONFIGURE`：设置参数。
- `AUDIO_CTL_START` / `AUDIO_CTL_STOP`：参数为 `int *`，取值 `AUDIO_STREAM_REPLAY`（0）或 `AUDIO_STREAM_RECORD`（1）。
- `AUDIO_CTL_GETBUFFERINFO`：在头文件中定义，**当前 `dev_audio.c` 的 `_audio_dev_control` 未实现该分支**；若应用依赖，需在框架或驱动中补全。

另有一组 **`CODEC_CMD_*`** 宏（音量、采样率、EQ 等），通常由底层 `configure`/私有 control 使用，与上述 `AUDIO_CTL_*` 不在同一命名空间，由具体驱动约定。

### 3.2 `struct rt_audio_ops`

| 回调 | 作用 |
|------|------|
| `getcaps` | 按 `rt_audio_caps` 的 `main_type` / `sub_type` 填写能力或当前值 |
| `configure` | 应用所选配置到硬件 |
| `init` | 设备 `init` 阶段调用一次 |
| `start` / `stop` | 按 `stream`（回放/录音）启停 DMA 或时钟 |
| `transmit` | **回放**：从框架维护的 `buf_info.buffer` 取 `size` 字节送到硬件（或触发 DMA）；`writeBuf` 指向当前硬件 DMA 窗口，`readBuf` 在此路径为 `NULL` |
| `buffer_info` | 填写 `struct rt_audio_buf_info`：`buffer` 指针、`block_size`、`block_count`、`total_size`（环形 DMA 缓冲区总长度） |

### 3.3 `struct rt_audio_buf_info`

硬件侧（或双缓冲）**环形缓冲区**描述：`buffer` + `block_size` × 逻辑块数，总长度 `total_size`。框架用 `replay->pos` 在其上滑动，按块调用 `transmit`。

### 3.4 `struct rt_audio_replay`

- **`mp`**：`rt_mp_create(..., RT_AUDIO_REPLAY_MP_BLOCK_COUNT, RT_AUDIO_REPLAY_MP_BLOCK_SIZE)`，与应用 `write` 拼块对应。
- **`queue`**：`rt_data_queue_init(..., CFG_AUDIO_REPLAY_QUEUE_COUNT, ...)`，默认 **4** 个槽位，存放已拼满的 mempool 块指针。
- **`lock`**：保护 `write` 路径上的拼块与入队。
- **`cmp`**：停止回放时等待 ISR 把剩余帧“走空”后完成同步。
- **`write_data` / `write_index` / `read_index` / `pos` / `event`**：应用侧写指针、队列 peek 消费偏移、硬件环形写指针、停止事件标志。

### 3.5 `struct rt_audio_record`

仅含 **`struct rt_audio_pipe pipe`** 与 **`activated`**。录音数据由驱动在 ISR 中 **`rt_audio_rx_done()`** 写入 Pipe。

### 3.6 `rt_audio_samplerate_to_speed()`

在 `dev_audio.c` 中实现：将 `AUDIO_SAMP_RATE_*` 位掩码对应的**标量 Hz** 返回；其中 `AUDIO_SAMP_RATE_11K` 映射 **11052**、`AUDIO_SAMP_RATE_172K` 映射 **176400**（与常见 11.025 kHz / 176.4 kHz 一致）。

---

## 4. 录音管道（`dev_audio_pipe.c` / `dev_audio_pipe.h`）

### 4.1 标志位 `enum rt_audio_pipe_flag`

可组合使用：

- **`RT_PIPE_FLAG_BLOCK_RD`**：读侧无数据时挂起读线程，有数据或写入后唤醒。
- **`RT_PIPE_FLAG_BLOCK_WR`**：写侧缓冲区满时挂起写线程（与 `FORCE_WR` 互斥语义见下）。
- **`RT_PIPE_FLAG_FORCE_WR`**：满时使用 **`rt_ringbuffer_put_force`**，可能覆盖未读数据；此时 **`BLOCK_WR` 被忽略**，写总能推进。

框架初始化录音 Pipe 时使用：

```c
(rt_int32_t)(RT_PIPE_FLAG_FORCE_WR | RT_PIPE_FLAG_BLOCK_RD)
```

含义：**写（来自 ISR）永不因满而阻塞，可能丢最旧数据**；**读侧阻塞**直到有数据。

### 4.2 读路径 `rt_audio_pipe_read`

- 非阻塞读：关中断 `ringbuffer_get`，若有读出量则尝试 **`_rt_audio_pipe_resume_writer`** 唤醒因满而挂起的写者（此处写者一般指阻塞写模式，与 `FORCE_WR` 组合时写很少阻塞）。
- 阻塞读：循环直至 `ringbuffer_get` 非 0，否则当前线程挂到 **`suspended_read_list`** 并 `rt_schedule()`。

### 4.3 写路径 `rt_audio_pipe_write`

- 若 **`FORCE_WR`** 或 **未** 设 **`BLOCK_WR`**：关中断下 `put_force` 或 `put`，然后 **`_rt_audio_pipe_resume_reader`**（可选 `rx_indicate`，并唤醒阻塞读线程）。
- 否则为阻塞写：缓冲区满则挂到 **`suspended_write_list`**。

### 4.4 `PIPE_CTRL_GET_SPACE`

`control()` 中若 `cmd == PIPE_CTRL_GET_SPACE` 且 `args` 非空，将 **`rt_ringbuffer_space_len`** 写入 `*(rt_size_t *)args`。

### 4.5 生命周期 API

- **`rt_audio_pipe_init`**：初始化挂起链表、`rt_ringbuffer_init`、注册为 **`RT_Device_Class_Pipe`** 设备。
- **`rt_audio_pipe_detach`**：`rt_device_unregister`。
- **`rt_audio_pipe_create` / `rt_audio_pipe_destroy`**（`RT_USING_HEAP`）：堆上分配 `struct rt_audio_pipe` 与 ring 内存，便于动态创建。

### 4.6 `struct rt_audio_portal_device`

在 **`dev_audio_pipe.h`** 中声明，**本目录 `.c` 未使用**，可视为预留或与其他组件组合的门户设备抽象。

---

## 5. 核心逻辑（`dev_audio.c`）

### 5.1 注册 `rt_audio_register()`

- 设置 `device->type = RT_Device_Class_Sound`。
- 绑定 `init/open/close/read/write/control`（或 `RT_USING_DEVICE_OPS` 下的 `audio_ops`）。
- **`rt_device_register(..., flag | RT_DEVICE_FLAG_REMOVABLE)`**，随后 **`rt_device_init()`** 触发 `_audio_dev_init`。

`flag` 应包含 **`RT_DEVICE_FLAG_RDONLY`** 和/或 **`RT_DEVICE_FLAG_WRONLY`**，以决定分配 **`record`** / **`replay`** 子结构。

### 5.2 `_audio_dev_init()`

1. 若 **`RT_DEVICE_FLAG_WRONLY`**：分配 **`rt_audio_replay`**，创建 mempool、数据队列、互斥量；`replay->activated = RT_FALSE`。
2. 若 **`RT_DEVICE_FLAG_RDONLY`**：分配 **`rt_audio_record`**，**`rt_malloc(RT_AUDIO_RECORD_PIPE_SIZE)`** 作 ring 内存，**`rt_audio_pipe_init(..., "record", FORCE_WR|BLOCK_RD, ...)`**。
3. 调用 **`ops->init`**。
4. 若存在 **`ops->buffer_info`**，则调用 **`buffer_info(audio, &audio->replay->buf_info)`**。

注意：当设备为**仅录音**（只设 `RDONLY`、未设 `WRONLY`）时，**`audio->replay` 为 `NULL`**，若仍提供 **`buffer_info`** 回调，**第 4 步会对 `replay` 解引用导致空指针**。实际 BSP 多为全双工或带回放，但若要做“纯 Mic”设备，需避免在 `replay == NULL` 时调用 `buffer_info`，或扩展框架。

### 5.3 `open` / `close`

- **`open`**：校验 `oflag` 与设备 `flag`；写打开时重置回放状态位；读打开时若未激活则 **`_audio_record_start()`**（内部会 **`rt_device_open` Pipe** 且 **`ops->start(RECORD)`**），并置 **`record->activated`**。
- **`close`**：若曾写打开则 **`_aduio_replay_stop()`**；若曾读打开则 **`_audio_record_stop()`**。

（源码中 `_aduio_replay_start` / `_aduio_replay_stop` 为拼写笔误。）

### 5.4 回放 `write`：`_audio_dev_write()`

在 **`replay->lock`** 下：

1. 按 **`RT_AUDIO_REPLAY_MP_BLOCK_SIZE`** 从 mempool 取块，将用户数据拷入 **`write_data`**，凑满一块则 **`rt_data_queue_push`** 整块指针与长度。
2. 若 **`replay->activated`** 仍为假，则 **`_aduio_replay_start()`**（`ops->start(REPLAY)`），并置 **`activated`**。

返回已写入字节数。首次真正往硬件送数往往还依赖 **`rt_audio_tx_complete()`** 链式触发（见下）。

### 5.5 回放硬件喂数：`_audio_send_replay_frame()`

由 **`rt_audio_tx_complete()`** 调用，典型场景是 **DMA 半块/整块传输完成中断** 里调用。

逻辑概要：

1. 取 **`buf_info`**，保存当前 **`pos`**，本帧长度为 **`block_size`**。
2. **`rt_data_queue_peek`**：若队列为空，则对当前硬件窗口 **`rt_memset` 为静音**，推进 **`pos`**（环形取模）；若 **`event` 含 `REPLAY_EVT_STOP`**，则 **`rt_completion_done(&replay->cmp)`** 通知 **`_aduio_replay_stop()`** 侧。
3. 若有数据：先清零当前块，再循环从队列 peek 的数据拷贝到 **`buf_info.buffer[pos...]`**，消费满一块则 **`rt_data_queue_pop` + `rt_mp_free`**，并可选调用 **`parent.tx_complete(&parent, (void *)data)`** 通知“某 mempool 块已归还逻辑”（注意此处传入的是**已弹出**的指针）。
4. 若实现了 **`ops->transmit`**，则以 **`&buf_info.buffer[position]`** 为 `writeBuf` 调用，长度 **`dst_size`**；返回值不等于 `dst_size` 则置 **` -RT_ERROR`**。

下溢（under-run）时会对 **`pos`**、`read_index` 做回退/重置并返回 **`-RT_EEMPTY`**（见 `LOG_D("under run"...)` 分支）。

### 5.6 停止回放：`_aduio_replay_stop()`

1. **`_audio_flush_replay_frame`**：若 **`write_index`** 非 0，将尾部未凑满块 **`rt_data_queue_push`**。
2. 置 **`REPLAY_EVT_STOP`**，**`rt_completion_init` + `wait`  forever** 等待 ISR 路径在送出零帧后 **`completion_done`**。
3. 清除 stop 标志，**`ops->stop(REPLAY)`**，**`activated = RT_FALSE`**。

### 5.7 录音 `_audio_record_start` / `_audio_record_stop`

- **start**：**`rt_device_open(Pipe, RDONLY)`**，**`ops->start(RECORD)`**。
- **stop**：**`ops->stop(RECORD)`**，**`rt_device_close(Pipe)`**。

### 5.8 录音上行：`rt_audio_rx_done()`

- **`rt_device_write(&record->pipe, 0, pbuf, len)`** 将 ISR 采到的数据压入 Pipe（`FORCE_WR` 下不阻塞）。
- 若 **`audio->parent.rx_indicate`** 非空，则 **`rx_indicate(&parent, len)`**。

应用侧通过 **`rt_device_read` 到 `audio` 设备** 时，实际 **`_audio_dev_read`** 转发为 **`rt_device_read(&record->pipe, ...)`**。

### 5.9 `control`：`_audio_dev_control()`

分发 **`AUDIO_CTL_GETCAPS`**、**`CONFIGURE`**、**`START`**、**`STOP`** 到上述内部函数或 **`ops`**；其他 `cmd` 返回 **`RT_EOK`** 且不处理。

---

## 6. BSP 驱动典型对接步骤（阅读源码时的对照清单）

1. 静态或动态分配 **`struct rt_audio_device`**，填好 **`ops`**。
2. **`rt_audio_register(audio, "sound0", RT_DEVICE_FLAG_WRONLY | RT_DEVICE_FLAG_RDONLY, user_data)`**（按实际能力裁剪 flag）。
3. 在 **`buffer_info`** 中给出与 DMA 一致的环形 **`buffer`** 与 **`block_size`**（通常为一帧或半帧字节数）。
4. 回放：**DMA 发送完成回调/中断** 末尾调用 **`rt_audio_tx_complete(audio)`**，以便框架调用 **`transmit`** 填下一窗。
5. 录音：**DMA 接收完成** 将数据 **`rt_audio_rx_done(audio, buf, len)`** 写入 Pipe。
6. 参数与音量：**`AUDIO_CTL_GETCAPS` / `CONFIGURE`** 或驱动私有 **`control`** 与 Codec 手册对齐。

---

## 7. 小结

| 项目 | 说明 |
|------|------|
| 目录职责 | 音频 **类设备框架**：Sound 设备 ops、回放 mempool+数据队列、录音 ringbuffer Pipe |
| 回放数据流 | 应用 `write` → mempool 块 → `data_queue` → `tx_complete` 链 → `transmit` → 硬件 |
| 录音数据流 | 硬件 → `rx_done` → Pipe 写 → 应用 `read` |
| 依赖 | 标准 RT-Thread 设备、IPC（mempool、mutex、completion、data queue）、ringbuffer |
| 阅读延伸 | 具体 BSP 的 `drv_sound.c` / Codec 驱动，对照本框架四个入口：`register`、**`transmit`**、**`tx_complete`**、**`rx_done`** |

以上覆盖了 `components/drivers/audio` 在 RT-Thread 5.2.0 中的职责边界与关键调用关系，便于从应用向下或从 BSP 向上对照阅读。
