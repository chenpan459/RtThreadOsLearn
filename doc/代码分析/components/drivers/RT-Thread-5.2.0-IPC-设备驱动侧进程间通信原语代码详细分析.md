# RT-Thread 5.2.0 Device IPC 目录代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/ipc` 下的 **设备驱动侧进程/线程间协作原语**：环形缓冲、完成量、数据队列、等待队列、工作队列、条件变量与管道等。头文件位于 **`components/drivers/include/ipc/`**，由 **`rtdevice.h`** 统一包含（与内核 **`src/ipc`** 中的信号量/邮箱等并列存在，职责不同）。

涉及路径：

- 实现：`ipc/*.c`
- 头文件：`include/ipc/*.h`
- 构建：`ipc/Kconfig`、`ipc/SConscript`

---

## 1. 总览与编译条件

```text
rtdevice.h
  → ipc/ringbuffer.h, completion.h, dataqueue.h, workqueue.h,
     condvar.h, waitqueue.h, pipe.h, poll.h, ringblk_buf.h
```

**`SConscript`**：

- 使用 **`Glob('*.c')`** 编译目录下全部 **`.c`**。
- **`depend = ['RT_USING_DEVICE_IPC']`**，并定义 **`LOCAL_CPPDEFINES=['__RT_IPC_SOURCE__']`**（供部分内联或条件编译区分“驱动 IPC 实现单元”）。
- **未** 开启 **`RT_USING_HEAP`** 时移除 **`dataqueue.c`、`pipe.c`**（二者依赖动态分配）。
- **`RT_USING_SMP`**：编 **`completion_mp.c`**，移除 **`completion_up.c`**；非 SMP 则相反。完成量核心逻辑因此分为 **UP** 与 **MP** 两套。

**`Kconfig`（`RT_USING_DEVICE_IPC`）**：

- **`RT_UNAMED_PIPE_NUMBER`**：无名管道实例数量上限（默认 64），与 **`pipe.c`** 中 POSIX 路径的资源 ID 管理相关。
- **`RT_USING_SYSTEM_WORKQUEUE`**：系统默认工作队列线程（栈大小、优先级可配）。

---

## 2. 环形字节缓冲（`ringbuffer.c` / `ringbuffer.h`）

**`struct rt_ringbuffer`**：单块连续 **`pool`**，**`read_index`/`write_index`** 各带 **1 bit mirror**，用“实缓冲区 + 虚拟镜像”区分空/满，避免额外长度字段。

**特点**（头文件明确写出）：**无** 线程阻塞/唤醒，仅内存与索引操作；多线程使用需外层加锁。

**API**：**`init`/`reset`、`put`/`put_force`、`putchar`、`get`/`getchar`、`peek`、`data_len`**；**`RT_USING_HEAP`** 下 **`create`/`destroy`**。

**典型用途**：串口 DMA 环形收包、设备 **`read`/`write`** 中间层缓存。

---

## 3. 环形块缓冲 RBB（`ringblk_buf.c` / `ringblk_buf.h`）

**`struct rt_rbb`**：大块 **`buf`** 切分为 **`rt_rbb_blk`** 链表节点（**`rt_slist_t`**），每块带 **`status`（UNUSED/INITED/PUT/GET）** 与 **`size`**，由 **`rt_spinlock`** 保护。

**API**：**`rt_rbb_init`/`create`、`blk_alloc` → 填数据 → `blk_put` → 消费者 `blk_get` → `blk_free`**；**`blk_queue_*`** 用于按序取出**连续多块**视图（零拷贝块链）。

**典型用途**：DMA 块提交、按帧组装的通信缓冲。

---

## 4. Completion（`completion.h` + `completion_comm.c` + `completion_up.c` / `completion_mp.c`）

**`struct rt_completion`**：单字 **`rt_atomic_t susp_thread_n_flag`**——**bit0** 为完成标志，**高位** 编码挂起线程指针（头文件注释中的 lockless 设计）。

**`completion_comm.c`**： **`rt_completion_done`**、**`rt_completion_wakeup`** 等对 **`rt_completion_wakeup_by_errno`** 的薄封装；**`rt_completion_wait`/`wait_noisr`** 转 **`rt_completion_wait_flags*`**。

**UP 实现（`completion_up.c`）**：静态 **`_completion_lock`**（自旋锁关中断路径），**`rt_completion_init`** 置未完成且无挂起线程；**`wait_flags`** 在未完成时挂起当前线程并记录到原子字；**`wakeup_by_errno`** 在完成或错误路径唤醒等待线程。

**API 语义**：**`wait_noisr`** 等标注 **不可在 ISR 与 completion 完成路径交叉于 ISR 调 `done`** 的约束（以注释为准）；**`wait_flags`** 系列可传 **`suspend_flag`**（与 **`rt_thread_suspend_with_flag`** 对齐）。

**用途**：工作队列线程 **`rt_completion_wait(&queue->wakeup_completion, …)`** 睡眠，提交 work 时 **`rt_completion_done`** 唤醒。

---

## 5. 数据队列 DataQueue（`dataqueue.c` / `dataqueue.h`）

**`struct rt_data_queue`**：定长槽 **`rt_data_item`**（**`const void *data_ptr` + `data_size`**），**`put_index`/`get_index`** 与 **空/满** 位域，**`spinlock`**，**`suspended_push_list` / `suspended_pop_list`**。

**`push`**：满则按 **`timeout`** 挂起到 push 链表；成功则 **`evt_notify(..., PUSH)`**，长度低于 **`lwm`** 时可 **`LWM`** 事件唤醒阻塞的 pop 侧写者（低水位语义见源码注释）。

**`pop`**：空则挂起；可 **`peek`** 只读不移除。

**依赖**：**`rt_data_queue_init`** 内 **`rt_malloc(size * sizeof(rt_data_item))`**，故无堆则不编译本文件。

**典型用途**：传感器数据帧、音频块等“指针+长度”投递。

---

## 6. 等待队列 Waitqueue（`waitqueue.c` / `waitqueue.h`）

**`struct rt_wqueue`**（类型名 **`rt_wqueue_t`**，定义在 **`rtdef.h`**）：**`waiting_list`**、**`spinlock`**、**`flag`（CLEAN/WAKEUP）**。

**`struct rt_wqueue_node`**：绑定 **`polling_thread`**、**`wakeup(wait,key)`** 回调、**`list`**。

**`rt_wqueue_wakeup`**：自旋锁内遍历，对每个节点调用 **`wakeup`**；返回 **0** 表示“可唤醒”，则 **`rt_thread_resume`** 并从链表摘除（**单次唤醒一个** 满足条件的节点）。**`rt_wqueue_wakeup_all`** 唤醒多个。

**`rt_wqueue_wait` / `_killable` / `_interruptible`**：在条件不满足时挂起并加入队列；与 **`rt_poll_add`**（见 **`poll.h`**）配合实现 **`select`/`poll`** 风格的多 fd 等待（**`poll.h`** 仅 **`rt_poll_add` 内联**，将 **`poll_queue_proc`** 注册到具体设备的 **`rt_wqueue_t`**）。

**`DEFINE_WAIT` / `DEFINE_WAIT_FUNC`**：栈上构造 **`rt_wqueue_node`**，默认 **`__wqueue_default_wake`** 恒返回 0（需自定义 **`wakeup`** 才能按 **`key`** 过滤）。

---

## 7. 工作队列 Workqueue（`workqueue.c` / `workqueue.h`）

**条件**：头文件与实现均包裹在 **`#ifdef RT_USING_HEAP`** 下。

**`struct rt_workqueue`**：**`work_list`**（待执行）、**`delayed_list`**（按 **`timeout_tick`** 排序的延迟 work）、**`sem`**（与 **`_workqueue_work_completion`** 配合做“执行完一次 ack”）、**`work_thread`**、**`spinlock`**、**`wakeup_completion`**。

**工作线程 `_workqueue_thread_entry`**：

1. 持锁将 **`delayed_list`** 中到期的 work 挪到 **`work_list`**（与 **`rt_tick_get()`** 比较，注意 **`RT_TICK_MAX/2`** 的环绕判断）。
2. 若 **`work_list`** 空，解锁后 **`rt_completion_wait(&wakeup_completion, delay_tick)`**。
3. 非空则取出队首 **`work_func(work, work_data)`**，再 **`_workqueue_work_completion`**（内部 **`rt_sem_trytake`/`release`** 维持信号量语义）。

**`rt_workqueue_submit_work(queue, work, ticks)`**：**`ticks==0`** 立即入 **`work_list`** 并 **`rt_completion_done`**；**`0 < ticks < RT_TICK_MAX/2`** 入 **`delayed_list`**；否则 **`-RT_ERROR`**。

**系统工作队列**：**`RT_USING_SYSTEM_WORKQUEUE`** 下提供 **`rt_work_submit`/`rt_work_urgent`/`rt_work_cancel`** 等，指向全局 **`sys_workqueue`**（初始化代码在同文件后部，**`INIT`** 导出）。

---

## 8. 条件变量 Condvar（`condvar.c` / `condvar.h`）

**`struct rt_condvar`**：**`rt_wqueue event`**、**`waiters_cnt`**、**`waiting_mtx`**（原子记录与 CV 绑定的 **`rt_mutex_t`**）。

**`rt_condvar_timedwait`**：调用前 **必须已持有 `mtx`**（**`CV_ASSERT_LOCKED`**）；内部把线程挂到 **`cv->event`**，**`rt_mutex_release(mtx)`**，唤醒后再 **`rt_mutex_take`**。同一 CV 在实现上要求 **同一 mutex** 配对使用（冲突时 **`-EBUSY`**）。

**`signal`/`broadcast`**：在持锁前提下 **`rt_wqueue_wakeup`** / **`rt_wqueue_wakeup_all`**，并清 **`event.flag`** 以减少虚假唤醒路径问题。

**`pipe`** 用 **`waitfor_parter`**：读端先打开且无写端时 **`rt_condvar_timedwait`** 等待写端 **`broadcast`**。

---

## 9. 管道 Pipe（`pipe.c` / `pipe.h`）

**`struct rt_pipe_device`**：**`rt_device`** 子类，**`rt_ringbuffer *fifo`**，**`reader_queue`/`writer_queue`**（**`rt_wqueue_t`**），读写端计数，**`rt_condvar waitfor_parter`**，**`rt_mutex lock`**。

**`rt_pipe_create`** 等：创建命名管道设备；**`RT_USING_POSIX_DEVIO` && RT_USING_POSIX_PIPE`** 时增加 **dfs vnode**、**`pipe()`** 无名管道、**`poll`** 与 **`RT_UNAMED_PIPE_NUMBER`** 资源表。

**读路径**：可阻塞、可中断（变更日志 2023-12）；**`poll`** 在写端关闭时返回 **`POLLHUP`**（见变更日志）。

**依赖**：堆上 **`rt_ringbuffer_create`**，故无堆不编 **`pipe.c`**。

---

## 10. Poll（`poll.h`）

仅提供 **`struct rt_pollreq`** 与 **`rt_poll_add(wq, req)`** 内联：若设备支持 **`poll`**，在 **`poll`** 入口通过 **`req->_proc`** 把当前 **`rt_wqueue_t`** 登记到 **`dfs`** 层 **`poll`** 框架。具体 **`_proc`** 实现在 **`dfs`** / **`devfs`** 侧，本目录 **无** `poll.c`。

---

## 11. 模块关系小结

| 模块 | 阻塞语义 | 同步手段 | 典型消费者 |
|------|----------|----------|------------|
| **ringbuffer** | 无 | 调用方自管 | 串口、通用缓存 |
| **ringblk_buf** | 无 | spinlock | DMA 块、帧缓冲 |
| **completion** | 有 | 原子字 + UP/MP 锁 | workqueue、驱动异步完成 |
| **dataqueue** | push/pop | spinlock + 挂起链表 | 传感器、流媒体指针传递 |
| **waitqueue** | `rt_wqueue_wait*` | spinlock + resume | 字符设备 read、poll |
| **workqueue** | 工作线程内 | spinlock + completion + sem | 中断底半部、延迟处理 |
| **condvar** | timedwait | wqueue + mutex 协议 | pipe 端配对 |
| **pipe** | read/write/open | mutex + wqueue + condvar | Shell、进程间字节流 |

---

## 12. 阅读与集成建议

1. **选原语**：仅要字节流且单生产者单消费者可考虑 **ringbuffer + 自建锁**；要 **线程挂起/超时** 用 **dataqueue** 或 **completion**；要 **defer 到线程上下文** 用 **workqueue**。
2. **SMP**：完成量务必走 **`completion_mp.c`** 路径，勿混用 UP 假设。
3. **资源**：无 **`RT_USING_HEAP`** 时无 **dataqueue/pipe/workqueue 动态创建**；可仅用 **静态 ringbuffer + completion** 等。
4. **POSIX 管道**：需同时打开 **`RT_USING_POSIX_DEVIO`** 与 **`RT_USING_POSIX_PIPE`**，并留意 **`RT_NAME_MAX`** 与 **`RT_UNAMED_PIPE_NUMBER`** 在 **`pipe.c`** 中的编译期检查。

阅读顺序：**`ringbuffer.h`**（数据结构最直观）→ **`completion.h` + `completion_up.c`** → **`waitqueue.c`** → **`workqueue.c`** → **`dataqueue.c`** → **`condvar.c`/`pipe.c`** → **`ringblk_buf.h`**。
