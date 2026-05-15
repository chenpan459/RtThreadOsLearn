# RT-Thread 5.2.0 SMP Call（跨核函数调用 / IPI 队列）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/smp_call` 目录实现的 **SMP 跨核调用框架**：在 **`RT_USING_SMP`** 下编译，通过 **每核一条软件请求队列 + `RT_SMP_CALL_IPI` 核间中断**，在目标 CPU 的 **IPI 处理函数** 上执行 **用户回调**；支持 **单核定向请求（用户自带 `rt_smp_call_req`）**、**按 CPU 掩码广播/组播（框架内嵌每发送方→每目标的槽位）**、**同步等待全部目标执行完（`SMP_CALL_WAIT_ALL`）** 与 **条件过滤（`rt_smp_cond_t`）**。

**说明**：公共头文件名为 **`smp_call.h`**，但 **include guard 为 `__SMP_IPI_H__`**，与文件名略不一致；**`rtdevice.h` 不自动包含本头文件**，一般由 **`libcpu`/BSP** 在 **SMP 初始化** 路径中包含并调用 **`rt_smp_call_init()`**、注册 **IPI 向量**。

---

## 1. 目录与编译

| 文件 | 作用 |
|------|------|
| **`smp_call.h`** | **`struct rt_smp_event` / `struct rt_smp_call_req`**、事件与 **flag** 宏、对外 API 声明 |
| **`smp_call.c`** | **每核数据结构、IPI 处理、请求入队、`rt_smp_call_*` 实现** |
| **`SConscript`** | **`RT_USING_SMP`** 时编译目录下 **`*.c`**，**`CPPPATH`** 指向本目录（**`#include "smp_call.h"`**） |

父级 **`components/drivers/SConscript`** 会遍历子目录并执行各自 **`SConscript`**，因此 **`smp_call`** 在 **开启 SMP** 时进入 **`DeviceDrivers`/`smp` 组**（与 **`DefineGroup('smp', ...)`** 命名一致）。

---

## 2. 与内核其余部分的衔接

### 2.1 IPI 号

**`include/rtdef.h`** 中默认：

```672:673:rt-thread-5.2.0/include/rtdef.h
#ifndef RT_SMP_CALL_IPI
#define RT_SMP_CALL_IPI                 2
```

平台可在 **`rtconfig.h`/板级头文件** 中覆盖 **`RT_SMP_CALL_IPI`**。**GIC** 路径下 **`components/drivers/pic/pic-gic-common.c`** 使用 **`DECLARE_GIC_IPI(RT_SMP_CALL_IPI, ...)`** 声明 SGI；**`pic.c`** 的 IPI 策略表中包含 **`RT_SMP_CALL_IPI`**。

### 2.2 初始化示例（AArch64）

**`libcpu/aarch64/common/setup.c`** 在 **`RT_USING_SMP`** 下：

```314:322:rt-thread-5.2.0/libcpu/aarch64/common/setup.c
#ifdef RT_USING_SMP
    rt_smp_call_init();
    /* Install the IPI handle */
    rt_hw_ipi_handler_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_STOP_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_SMP_CALL_IPI, rt_smp_call_ipi_handler);
    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);
#endif
```

即：**先 `rt_smp_call_init()`，再安装 `rt_smp_call_ipi_handler`，最后 umask 该向量**。

---

## 3. 数据结构（`smp_call.h`）

### 3.1 `struct rt_smp_event`

| 成员 | 含义 |
|------|------|
| **`event_id`** | **`SMP_CALL_EVENT_*`**：全局异步/全局同步/单次请求 |
| **`func` / `data`** | 目标核执行的 **回调与参数** |
| **`typed` 联合体** | **全局同步**：`calling_cpu_mask` 指向 **原子 CPU 位掩码**，每核执行完 **清本位**；**`rt_smp_call_request` 路径**：`usage_tracer` 作 **请求槽占用标记** |

### 3.2 `struct rt_smp_call_req`

| 成员 | 含义 |
|------|------|
| **`freed_lock`** | **自旋锁**：保证 **同一发送核→同一目标核** 的 **内嵌槽位** 在 **上一次 IPI 处理完毕** 前不会被复写 |
| **`event`** | 本次调用的 **事件与回调** |
| **`slist_node`** | 挂入 **目标 CPU 的 `call_queue`**（**`rt_ll_slist_t`**） |

### 3.3 每核全局表 `_smp_data_cores[RT_CPUS_NR]`

**`smp_call.c`** 中：

```19:26:rt-thread-5.2.0/components/drivers/smp_call/smp_call.c
static struct smp_data
{
    /* call request data to each cores */
    struct rt_smp_call_req call_req_cores[RT_CPUS_NR];

    /* call queue of this core */
    rt_ll_slist_t call_queue;
} _smp_data_cores[RT_CPUS_NR];
```

语义：

- **`_smp_data_cores[i].call_queue`**：**CPU `i` 上待执行的请求链表头**（IPI 到来时 **出队并处理**）。
- **`_smp_data_cores[sender].call_req_cores[t]`**：**发送核 `sender` 发往目标 `t` 的固定槽**（避免在栈上分配跨核请求；通过 **`freed_lock` + 事件类型** 串行化复用）。

---

## 4. 事件类型与 flag

| 宏 | 值 | 含义 |
|----|---|------|
| **`SMP_CALL_EVENT_GLOB_ASYNC`** | 0x1 | **掩码多核异步**：拷贝到本地 **`req_local` 后执行，不维护全核完成掩码** |
| **`SMP_CALL_EVENT_GLOB_SYNC`** | 0x2 | **掩码多核同步**：执行后 **`_mask_out_cpu`** 清除 **`calling_cpu_mask`** 中本核位；发起方 **自旋等待** 掩码与 **`cpu_mask` 交集为 0** |
| **`SMP_CALL_EVENT_REQUEST`** | 0x4 | **`rt_smp_call_request` 用户请求**：执行后 **`_call_req_release`**（**`usage_tracer`**） |

| flag | 含义 |
|------|------|
| **`SMP_CALL_WAIT_ALL`** | **`rt_smp_call_func_cond` / 掩码 API** 使用；**`rt_smp_call_request` 禁止与该 flag 组合**（返回 **`-RT_EINVAL`**） |
| **`SMP_CALL_NO_LOCAL`** | **不在本核直接调用**；即使 **`cpu_mask` 含本核** 也 **只发 IPI**（否则本地在 **关本地 IRQ** 下 **直接 `func(data)`** 模拟 ISR 环境） |
| **`SMP_CALL_SIGNAL`** | 头文件中保留；**`smp_call.c` 当前未引用**（可视为扩展预留） |

**`RT_ALL_CPU`**： **`(1 << RT_CPUS_NR) - 1`**。

---

## 5. 核心执行路径

### 5.1 IPI 入口 `rt_smp_call_ipi_handler`

- 断言处于 **中断嵌套**（**`rt_interrupt_get_nest()`** 非 0）。
- **`oncpu = rt_hw_cpu_id()`**，循环 **`rt_ll_slist_dequeue(&_smp_data_cores[oncpu].call_queue)`**，对每个 **`rt_smp_call_req`** 调用 **`_smp_call_handler(request, oncpu)`**。

### 5.2 `_smp_call_handler`

| `event_id` | 行为 |
|-------------|------|
| **`GLOB_SYNC` / `GLOB_ASYNC`** | **`_do_glob_request`**：把队列里的 **`req_global` 拷贝到栈上 `req_local`**，**`rt_hw_spin_unlock(&req_global->freed_lock)`** 释放发送方自旋锁；再 **`event->func(data)`**。仅 **SYNC** 时随后 **`_mask_out_cpu`** 更新 **`calling_cpu_mask`**。 |
| **`REQUEST`** | **`_do_request`**：直接 **`func(data)`** 后 **`_call_req_release`**。 |

**`_do_glob_request`** 中 **`RT_ASSERT(!!event->func)`**：全局路径 **必须** 提供函数指针。

### 5.3 `_smp_call_func_cond`（`rt_smp_call_cpu_mask*` / `each_cpu*` 的公共实现）

1. **`RT_ASSERT(!rt_hw_interrupt_is_disabled())`**：要求 **调用点未关全局中断**（与 **`rt_smp_call_request` 可在 ISR/关中断** 形成对比）。
2. **`rt_enter_critical`** 后取 **`oncpu`**。
3. 若 **`cpu_mask` 含本核** 且 **未** 设 **`SMP_CALL_NO_LOCAL`**：记下 **`call_local`**，并从 **`cpu_mask` 去掉本核**（本核稍后 **本地关中断执行**）。
4. 对 **`cpu_mask` 中每一位 `tmp_id`**（**`__rt_ffsl`** 遍历）：
   - 若 **`cond` 存在且 `cond(tmp_id, data)==RT_FALSE`**：从 **`cpu_mask` 清除该位**，不计入 **`rcpu_cnt`**，不发该核。
   - 否则 **`rt_hw_spin_lock(&call_req->freed_lock)`** 其中 **`call_req = &_smp_data_cores[oncpu].call_req_cores[tmp_id]`**（**占槽等待**），填写 **`event`（含 `GLOB_SYNC`/`ASYNC` 与共享 `calling_cpu_mask`）**，**`enqueue` 到 `_smp_data_cores[tmp_id].call_queue`**，**`rcpu_cnt++`**。
5. 若仍有 **`cpu_mask`**：**`rt_hw_ipi_send(RT_SMP_CALL_IPI, cpu_mask)`** 一次（**批量 IPI**）。
6. **`call_local`**：在 **`local_irq_disable` 下 `func(data)`**。
7. **`SMP_CALL_WAIT_ALL` 且 `rcpu_cnt>0`**：**`while (rt_atomic_load(maskp) & cpu_mask)`** 忙等，直至所有目标核清完掩码。

### 5.4 `rt_smp_call_request`（用户自带 `call_req`）

- 若 **`usage_tracer` 已为 BUSY**：**`-RT_EBUSY`**。
- **`SMP_CALL_WAIT_ALL`**：**`-RT_EINVAL`**。
- **`rt_enter_critical`** 后：
  - **本核 == 目标** 且 **未** `NO_LOCAL`：**关本地 IRQ 直接调 `func`**（不经过队列）。
  - **否则**：**`_call_req_take`** → **入目标核队列** → **`rt_hw_ipi_send`**。
- **`rt_exit_critical_safe`**。

**注意**：该 API **不会在返回前等待远端执行完**；若需与 **`REQUEST` + `usage_tracer`** 配对释放，可另调 **`rt_smp_request_wait_freed`**（**线程上下文**，内部 **`rt_thread_yield`** 轮询原子变量）。

### 5.5 `rt_smp_call_req_init`

将 **`event_id` 置为 `SMP_CALL_EVENT_REQUEST`**，**`usage_tracer` 清零**，设置 **`func/data`**。典型与 **`rt_smp_call_request`** 或自管生命周期配合。

### 5.6 `rt_smp_call_init`

**`memset` 全局表**，并对 **所有 `(i,j)`** 初始化 **`_smp_data_cores[i].call_req_cores[j].freed_lock`**。

---

## 6. API 速查

| 函数 | 说明 |
|------|------|
| **`rt_smp_call_each_cpu` / `_cond`** | **`cpu_mask = RT_ALL_CPU`** |
| **`rt_smp_call_cpu_mask` / `_cond`** | 任意 **掩码** |
| **`rt_smp_call_func_cond`** | 上述两者的底层实现 |
| **`rt_smp_call_request`** | **单目标核 + 用户 `rt_smp_call_req`**，适合 **ISR / 关中断** |
| **`rt_smp_request_wait_freed`** | **线程** 等待 **`REQUEST` 类 `usage_tracer` 释放** |

---

## 7. 使用与实现注意

1. **回调上下文**：队列请求在 **IPI ISR** 中执行，**应短小、不可阻塞**；勿持与 **发起线程** 形成死锁的锁。
2. **`rt_smp_call_func_cond` 族**：禁止在 **关中断** 下调用；内部依赖 **`rt_enter_critical`** 与 **可能的长自旋（`WAIT_ALL`）**。
3. **槽位与并发**：**`call_req_cores[sender][target]`** 保证 **同一 (sender→target) 同时仅一条 in-flight 全局/槽位请求**；不同发送核对同一目标可并行（各自槽位）。
4. **`SMP_CALL_SIGNAL`**：当前 **`.c` 未使用**，勿依赖其语义，除非后续版本实现。
5. **本地 + `cond`**：`_smp_call_func_cond` 在 **`call_local`** 分支使用 **`cond(tmp_id, data)`**，而 **`tmp_id` 为循环中最后一次赋值的远端 CPU 索引**，若使用 **`cond` 且同时 **`call_local`**，需自行确认是否满足预期（更自然的本地条件一般是 **`oncpu`**）。

---

## 8. 小结

| 组件 | 职责 |
|------|------|
| **`smp_call.h`** | 类型与常量定义 |
| **`smp_call.c`** | **IPI 队列调度、同步屏障位图、用户请求占用标记** |
| **BSP / `libcpu`** | **`rt_smp_call_init` + `rt_hw_ipi_handler_install(RT_SMP_CALL_IPI, ...)`** |

该子系统是 **RT-Thread SMP 上“在指定核跑一小段 C 函数”** 的基础设施，常用于 **TLB/Cache 维护、每核变量访问、驱动 per-CPU 状态** 等场景。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/smp_call/`。*
