# RT-Thread 5.2.0 KTIME 内核时间子系统代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/ktime` 目录实现的 **Ktime（kernel time）**：在 **`RT_USING_KTIME`** 下提供 **启动时间（boottime）**、**CPU 单调计时器（cputimer）** 抽象，以及基于 **`cputimer` 计数域** 的 **高精度软件定时器链表（hrtimer）** 与 **可中断睡眠延时**。头文件为 **`ktime/inc/ktime.h`**（通过子目录 **`CPPPATH`** 以 **`#include "ktime.h"`** 引用）；**`rtdevice.h` 不自动包含 `ktime.h`**，需按需 **`#include <ktime.h>`**（例如 **`pic.c`、`dev_soft_rtc.c`**）。

官方说明见同目录 **`README.md`**；下文侧重源码结构与实现要点。

---

## 1. 目录与编译

| 路径 | 作用 |
|------|------|
| **`inc/ktime.h`** | 对外 API、`struct rt_ktime_hrtimer`、`RT_KTIME_RESMUL` |
| **`src/boottime.c`** | **`rt_ktime_boottime_get_*`**（**`rt_weak`**） |
| **`src/cputimer.c`** | **`rt_ktime_cputimer_*`** 默认弱实现（退化为 **tick**） |
| **`src/hrtimer.c`** | 定时器链表、**`rt_ktime_hrtimer_process`**、睡眠与 **`rt_ktime_hrtimer_settimeout`** 弱实现 |
| **`src/aarch64/cputimer.c`** | AArch64：**`gtimer.h`** 的 **`rt_hw_get_gtimer_frq` / `rt_hw_get_cntpct_val`** |
| **`src/risc-v/virt64/cputimer.c`** | RISC-V virt64：**`rdtime`**、**`CPUTIME_TIMER_FREQ`** |

**`SConscript`**：

- 基础：**`Glob('src/*.c')`**（**`boottime`、`cputimer`、`hrtimer`**）。
- 架构相关：若存在 **`src/<ARCH>/<CPU>/`** 则追加其下 **`.c`**，否则 **`src/<ARCH>/*.c`**。
- **`CPPPATH`**：模块根目录 + **`inc`**。
- **`depend=['RT_USING_KTIME']`**；GNU C99 / armcc 选项与工程一致。

**`Kconfig`**：仅 **`menuconfig RT_USING_KTIME`**（默认 **n**）。其它模块（如 **`pic/Kconfig`**）可 **`depends on RT_USING_KTIME`**。

**上层集成**：**`components/drivers/SConscript`** 遍历子目录，凡含 **`SConscript`** 即参与编译，**`ktime`** 独立为 **`DefineGroup('ktime', ...)`** 组。

---

## 2. 时间标度：`RT_KTIME_RESMUL`

**`#define RT_KTIME_RESMUL (1000000ULL)`**

**`rt_ktime_cputimer_getres()` / `rt_ktime_hrtimer_getres()`** 的返回值语义为：**每增加 1 个 `cnt` 所对应的时间（秒）× `RT_KTIME_RESMUL`**（头文件注释写作 **`(resolution * RT_KTIME_RESMUL)`**）。

**`boottime.c`** 中换算纳秒：

```c
ns = (rt_ktime_cputimer_getcnt() * rt_ktime_cputimer_getres()) / RT_KTIME_RESMUL;
```

即：**`ns = cnt ×（每 cnt 的秒分辨率）× 1e9`**，与弱实现里 **`getres = 1e9×REsmul/TICK_PER_SECOND`**（tick 源）自洽。

---

## 3. Cputimer（`cputimer.c` + 架构文件）

### 3.1 弱默认（`src/cputimer.c`）

未由 BSP/架构覆盖时：

| API | 行为 |
|-----|------|
| **`getres`** | **`(10^9 × RT_KTIME_RESMUL) / RT_TICK_PER_SECOND`** |
| **`getfrq`** | **`RT_TICK_PER_SECOND`** |
| **`getcnt`** | **`rt_tick_get()`** |
| **`getstep`** | **1**（每个 OS tick 对应 cputimer 计数 +1） |
| **`init`** | 空 |

即 **cputimer 与 OS tick 对齐**，精度为 **tick 粒度**。

### 3.2 AArch64（`src/aarch64/cputimer.c`）

- **`getfrq`**：**`rt_hw_get_gtimer_frq()`**（物理计数器频率）。
- **`getcnt`**：**`rt_hw_get_cntpct_val() - _init_cnt`**（启动时基）。
- **`getstep`**：**`getfrq() / RT_TICK_PER_SECOND`**（每 tick 对应的计数增量）。
- **`init`**：记录 **`_init_cnt`**，使 **`getcnt`** 从 0 起算。

### 3.3 RISC-V virt64（`src/risc-v/virt64/cputimer.c`）

- **`rdtime`** 读 **`time`** CSR，减去 **`_init_cnt`**。
- 频率宏 **`CPUTIME_TIMER_FREQ`**（编译期常量）。

**BSP 适配**：其它 SoC 可在 BSP 或 **`libcpu`** 中提供同名强符号覆盖 **`cputimer.c`** 的 **`rt_weak`** 函数；**`README.md`** 亦说明 **STM32** 等需自行对接（**`boottime` 全为 weak**，便于整体替换）。

---

## 4. Boottime（`boottime.c`）

三个 **`rt_weak`** 接口均基于 **`cputimer`**：

- **`rt_ktime_boottime_get_us`** → **`struct timeval`**
- **`rt_ktime_boottime_get_s`** → **`time_t` 秒**
- **`rt_ktime_boottime_get_ns`** → **`struct timespec`**

语义：**从上电/调用 `rt_ktime_cputimer_init` 清零基线以来**的单调时间（取决于 **`getcnt`** 是否减初值），**非墙上时钟**。**`README.md`** 提醒 tick 中断到 **`rt_tick_set`/`rt_tick_increase`** 之间的延迟会引入误差。

---

## 5. Hrtimer（`hrtimer.c`）

### 5.1 数据结构

**`struct rt_ktime_hrtimer`**：兼容 **`rt_timer` 风格 flag**、**`name`**、链表节点 **`node`**、**`delay_cnt`/`timeout_cnt`（均在 `rt_ktime_cputimer_getcnt()` 同一计数域下）**、**`error`**、**`struct rt_completion completion`**、超时回调 **`timeout_func(parameter)`**。

### 5.2 全局链表与锁

- **`_timer_list`**：按 **`timeout_cnt` 升序**插入（**`_insert_timer_to_list_locked`**），等价于“最近超时在前”的优先级队列（**`README.md`** 注明后续可能改为红黑树）。
- **`_spinlock`**：保护链表与 **`_set_next_timeout_locked`** 路径。

### 5.3 弱函数：硬件时间轴与“下一次中断”

| 弱符号 | 默认行为 |
|--------|----------|
| **`rt_ktime_hrtimer_getres/getfrq/getcnt`** | 与 **tick** 一致（同 **`hrtimer_getres`** 弱实现） |
| **`rt_ktime_hrtimer_settimeout(cnt)`** | 使用静态 **`struct rt_timer`** **单次**定时，回调 **`rt_ktime_hrtimer_process`**，即 **软件 tick 驱动** 的 hrtimer 推进 |

BSP 若对接 **真实硬件定时器**，应重写 **`get*`** 与 **`settimeout`**：在 **`settimeout`** 里把 **`cnt`（hrtimer 计数单位）** 写入比较器，在 **ISR** 末尾调用 **`rt_ktime_hrtimer_process()`**。

### 5.4 `_cnt_convert`

将 **“cputimer 绝对时刻 `timer->timeout_cnt`”** 转为 **“当前到点还需多少个 hrtimer 计数”**：

- **`count = timer->timeout_cnt - rt_ktime_cputimer_getcnt()`**；若差值过大（环绕保护 **`_HRTIMER_MAX_CNT/2`**）返回 **0**（由调用方立刻 **`_hrtimer_process_locked`**）。
- **`rtn = count * getres(cputimer) / getres(hrtimer)`**，至少为 **1**。

### 5.5 `rt_ktime_hrtimer_process`

关自旋锁后：**`_hrtimer_process_locked`**（弹出所有 **`timeout_cnt <= cputimer_getcnt()`** 的节点，执行 **`timeout_func`**；周期定时器重新计算 **`timeout_cnt`** 并插回链表），再 **`_set_next_timeout_locked`**（对队首调用 **`rt_ktime_hrtimer_settimeout`**，若已过期则循环处理）。

**注意**：**`timeout_func` 仍在调用 `rt_ktime_hrtimer_process` 的上下文执行**（弱默认下为 **`rt_timer` 超时回调线程/软定时器上下文**；硬件 ISR 直连时则为中断上下文）。**`README.md`** 写明 **回调内勿做耗时操作**。

### 5.6 对外 API 摘要

- **`rt_ktime_hrtimer_init` / `start` / `stop` / `control` / `detach`**：语义贴近 **`rt_timer`**（含 **`RT_TIMER_CTRL_*`** 部分命令）。
- **`rt_ktime_hrtimer_keep_errno`**：写 **`timer->error`** 并 **`rt_set_errno(-err)`**。
- **延时**：**`delay_init`** 把 **`timeout_func`** 设为 **`_sleep_timeout`**（内部 **`rt_completion_done`**）；**`sleep(cnt)`** 以 **cputimer 计数** 睡眠；**`ndelay`/`udelay`/`mdelay`** 通过 **`ns × RT_KTIME_RESMUL / cputimer_getres()`** 转为 **`cnt`** 再 **`sleep`**。**`sleep`** 使用 **`rt_completion_wait_flags(..., RT_INTERRUPTIBLE)`**。

### 5.7 `rt_ktime_hrtimer_control` 边角

**`RT_TIMER_CTRL_GET_FUNC`** 分支写 **`arg = (void *)timer->timeout_func`**，仅修改局部指针，**无法** 通过 **`arg` 向调用方传出函数指针**（若需获取回调，应使用 **`void **`** 或单独 API）。属实现瑕疵，使用 **`control`** 时需注意。

---

## 6. 与 `README.md` 的差异

- **`README.md`** 写 **`rt_ktime_hrtimer_delete`**，头文件与实现仅有 **`rt_ktime_hrtimer_detach`**，以源码为准。
- **`README` 3.3.1 节号** 重复为两个 **3.3.1**，不影响代码理解。

---

## 7. 使用建议（驱动/BSP）

1. 先 **`rt_ktime_cputimer_init()`**（AArch64/RV 已在对应 **`cputimer.c`** 做基线；弱实现可忽略）。
2. 需要 **亚 tick 延时** 时：实现 **`rt_ktime_hrtimer_settimeout` + 与 cputimer 一致的计数换算**，并在定时器 **ISR** 调 **`rt_ktime_hrtimer_process`**。
3. **仅开 Ktime 不重写弱函数**：hrtimer 退化为 **tick 级**，与 **`rt_thread_mdelay`** 类能力接近，但多了 **completion 挂起** 与 **链表管理** 路径。
4. **墙上 RTC**：应使用 **`RTC`/`POSIX`/`settimeofday`** 等；**`boottime`** 只表示 **单调运行时间**。

---

## 8. 小结

| 层级 | 文件 | 职责 |
|------|------|------|
| 标度与声明 | **`ktime.h`** | **`RT_KTIME_RESMUL`、API、`rt_ktime_hrtimer`** |
| 单调基准 | **`cputimer*.c`** | 读 CPU 计时器或 tick |
| 启动时刻 | **`boottime.c`** | **`cnt×res` → 人类可读时间结构** |
| 高精度调度 | **`hrtimer.c`** | 有序链表 + **`settimeout` 弱钩子** + **`completion` 睡眠** |

阅读顺序：**`ktime.h`** → **`cputimer.c`**（弱语义）→ 本 **`ARCH/CPU` 的 `cputimer.c`** → **`boottime.c`** → **`hrtimer.c`**（**`_hrtimer_process_locked` / `_set_next_timeout_locked`**）。
