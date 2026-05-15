# RT-Thread 5.2.0 CPUTIME 高精度 CPU 时钟与 CPUTIMER 代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/cputime` 目录：提供 **CPU 周期级高精度计数**（`clock_cpu_gettime/getres`）、可选 **基于 CPU tick 的超时回调**（`clock_cpu_settimeout`），以及在此之上的 **软件 `rt_cputimer` 链表** 与 **`rt_cputime_*delay/sleep`** 封装。

涉及文件：

- 架构无关：`cputime.c`、`cputimer.c`
- Cortex-M：`cputime_cortexm.c`（DWT 周期计数或 **perf_counter**）
- RISC-V64：`cputime_riscv.c`（**`rdtime`**）
- 头文件：`components/drivers/include/drivers/cputime.h`、`cputimer.h`
- 配置与构建：`Kconfig`、`SConscript`

---

## 1. Kconfig 与 SConscript

### 1.1 `RT_USING_CPUTIME`

总开关；帮助信息说明：BSP 应通过 **`clock_cpu_setops()`** 注册 **`struct rt_clock_cputime_ops`**，应用可用 **`clock_cpu_gettime()`** 做差分，再用 **`clock_cpu_microsecond` / `clock_cpu_millisecond`** 换算。

### 1.2 子选项

| 选项 | 含义 |
|------|------|
| **`RT_USING_CPUTIME_CORTEXM`** | 依赖 **M0/M3/M4/M7**，**`select PKG_USING_PERF_COUNTER`**（与源码中 `#ifdef PKG_USING_PERF_COUNTER` 分支一致） |
| **`RT_USING_CPUTIME_RISCV`** | 依赖 **`ARCH_RISCV64`**，使用 **`rdtime`** |
| **`CPUTIME_TIMER_FREQ`** | RISC-V 路径下 **`riscv_cputime_getres()`** 使用的频率（**默认 0**，实际 BSP 需配置为非 0，否则分辨率计算无意义） |

### 1.3 `SConscript`

始终编译 **`cputime.c`、`cputimer.c`**；按选项追加 **`cputime_cortexm.c`** 或 **`cputime_riscv.c`**；**`CPPPATH`** 指向 **`components/drivers/include`**。

---

## 2. 操作表与全局入口（`cputime.h` / `cputime.c`）

### 2.1 `struct rt_clock_cputime_ops`

| 成员 | 语义 |
|------|------|
| **`cputime_getres`** | 返回 **每个 CPU tick 对应的纳秒数**（实现上为 `uint64_t`，注释写 ns/tick） |
| **`cputime_gettime`** | 返回当前 **单调递增的 CPU tick 计数** |
| **`cputime_settimeout`**（可选） | 在将来某一 **绝对 tick** 触发回调 **`timeout(param)`**；取消语义由实现约定（本框架在 **`cputimer.c`** 中用 **全 NULL** 调用表示关闭） |

### 2.2 `clock_cpu_setops(const struct rt_clock_cputime_ops *ops)`

- 设置静态指针 **`_cputime_ops`**。
- 若 **`ops` 非空**，**`RT_ASSERT`** 要求 **`getres` 与 `gettime` 非空`**；**`settimeout` 可为空**。

### 2.3 对外 API（`cputime.c`）

- **`clock_cpu_getres/gettime`**：未注册 ops 时 **`errno = ENOSYS`**，返回 **0**。
- **`clock_cpu_settimeout`**：未注册或 **ops 无 settimeout** 时同样 **ENOSYS**，返回 **0**。
- **`clock_cpu_issettimeout`**：若 ops 存在且 **`cputime_settimeout` 指针非空** 返回真，否则 **`RT_FALSE`**。
- **`clock_cpu_microsecond(cpu_tick)`** / **`clock_cpu_millisecond(cpu_tick)`**：  
  `unit = getres()`，再按  
  **us = (cpu_tick * unit) / (1000*1000) / 1000**，  
  **ms = (cpu_tick * unit) / (1000*1000) / (1000*1000)**。  
  即 **`unit` 被当作「每 tick 纳秒数」** 参与换算（与头文件注释一致）。

源码注释中 **`clock_cpu_seops`** 为 **`setops`** 的笔误。

---

## 3. Cortex-M 实现（`cputime_cortexm.c`）

### 3.1 `cortexm_cputime_getres`

**`1e9 * 1e6 / SystemCoreClock`**：在 **1GHz·ns 量纲** 下换算到 **每 DWT 周期对应的纳秒**（与 `cputime.c` 中除 **1000³** 的用法配套）。

### 3.2 `cortexm_cputime_gettime`

- **`PKG_USING_PERF_COUNTER`**：**`get_system_ticks()`**。
- 否则：**`DWT->CYCCNT`**。

### 3.3 `cortexm_cputime_init`（`INIT_BOARD_EXPORT`）

- **perf_counter 路径**：直接 **`clock_cpu_setops(&_cortexm_ops)`**。
- **否则**：检查 **`DWT_CTRL_NOCYCCNT`** 为 0（支持周期计数），置 **`CoreDebug->DEMCR.TRCENA`**，置 **`DWT_CTRL_CYCCNTENA`**，再 **`clock_cpu_setops`**。

**注意**：**`_cortexm_ops` 仅填充 `getres`/`gettime`，未实现 `cputime_settimeout`**。因此 **`clock_cpu_issettimeout()` 为假**，**`cputimer.c` 中依赖 settimeout 的路径不可用**（`rt_cputimer_init` 会 **`RT_ASSERT(clock_cpu_issettimeout())`**）。若需 **`rt_cputimer` / `rt_cputime_sleep`**，BSP 需在 **`rt_clock_cputime_ops`** 中自行实现 **`cputime_settimeout`**（例如用比较器 + 中断，或内核 tick 近似），再通过 **`clock_cpu_setops`** 合并或替换。

---

## 4. RISC-V64 实现（`cputime_riscv.c`）

- **`riscv_cputime_gettime`**：内联汇编 **`rdtime`**，读 **CSR time** 语义的时间寄存器（具体频率与 **`CPUTIME_TIMER_FREQ`** 一致）。
- **`riscv_cputime_getres`**：**`(1e9 * 1e6) / CPUTIME_TIMER_FREQ`**，与 Cortex-M 公式同形。
- **`riscv_cputime_init`**：**`INIT_BOARD_EXPORT`** 注册 **`_riscv_ops`**（同样 **无 `settimeout`**）。

---

## 5. 软件 CPUTIMER（`cputimer.c` + `cputimer.h`）

### 5.1 数据结构 `struct rt_cputimer`

- **`parent`**：`rt_object`，使用 **`RT_TIMER_FLAG_*`** 与 **`RT_TIMER_CTRL_*`** 与系统定时器部分命令值复用风格。
- **`row`**：插入全局有序链表 **`_cputimer_list`**。
- **`timeout_func` / `parameter`**：到期回调。
- **`init_tick`**：用户设定的相对 tick（**`RT_TIMER_CTRL_GET_TIME`** 返回它）。
- **`timeout_tick`**：**绝对到期 tick** = **`init_tick + clock_cpu_gettime()`**（在 init/set 时更新）。
- **`sem`**：供 **`rt_cputime_sleep`** 使用的同步原语。

### 5.2 全局状态

- **`_cputimer_list`**：按 **`timeout_tick`** 递增插入（**`rt_cputimer_start`** 中 for 循环查找插入点；用 **`(t->timeout_tick - timer->timeout_tick)`** 与 **`0x7fffffffffffffff`** 比较处理无符号环绕的写法，依赖 tick 空间足够大）。
- **`_cputimer_nowtimer`**：当前已交给硬件/ **`clock_cpu_settimeout`** 的那一个 **`rt_cputimer`**。

### 5.3 超时公共回调 `_cputime_timeout_callback`

关中断：清除 **`_cputimer_nowtimer`**，从链表摘除当前 timer，开中断后调用 **用户 `timeout_func`**。若链表非空，则取 **队首** 下一个 timer 再次 **`clock_cpu_settimeout`**；否则 **`clock_cpu_settimeout(RT_NULL, RT_NULL, RT_NULL)`** 表示取消底层超时。

### 5.4 `_set_next_timeout`

在 **链表头** 与 **`_cputimer_nowtimer`** 之间比较 **绝对 `timeout_tick`**，必要时重新 **`clock_cpu_settimeout`**，保证 **当前硬件等待的是全局最早到期** 的项。

### 5.5 API 摘要

| API | 作用 |
|-----|------|
| **`rt_cputimer_init`** | 校验 **`clock_cpu_issettimeout()`**，初始化字段，**`timeout_tick = tick + clock_cpu_gettime()`**，**`rt_sem_init`** |
| **`rt_cputimer_start/stop/delete/detach`** | 链表维护 + **`_set_next_timeout`**；**`detach`** 会 **`rt_sem_detach`** |
| **`rt_cputimer_control`** | **GET/SET_TIME**、**ONE_SHOT/PERIODIC**、**GET_STATE**、**GET_REMAIN_TIME**（写 **`timeout_tick`**）、**SET/GET_FUNC/PARM** 等 |

**实现注意**：**`RT_TIMER_CTRL_GET_FUNC`** 分支写 **`arg = (void *)timer->timeout_func`** 仅修改**局部变量 `arg`**，**无法把函数指针传出给调用方**，属明显缺陷；若需获取回调，应改为 **`*(void (**)(void *))arg = ...`** 或返回约定结构。

### 5.6 `rt_cputime_sleep(tick)`

若 **未实现 settimeout**：退化为 **`clock_cpu_millisecond(tick)` → `rt_thread_delay`**。

否则：栈上 **`rt_cputimer`**，**`init` + `start`**，**`rt_sem_take_interruptible`** 等待 **`_cputime_sleep_timeout`** 释放信号量，最后 **`detach`**。

### 5.7 `rt_cputime_ndelay/udelay/mdelay`

**`ndelay`**：**`rt_cputime_sleep(ns * (1e9) / clock_cpu_getres())`**（将纳秒换成 CPU tick 再 sleep）。**`udelay`/`mdelay`** 为 **ns 倍数** 包装。

---

## 6. 头文件 `cputimer.h` 与实现的小差异

- **`rt_cputimer_init` / `rt_cputimer_delete`** 在头文件里包在 **`#ifdef RT_USING_HEAP`** 下，但 **`cputimer.c`** 中 **`rt_cputimer_init`** 等**未**用 **`RT_USING_HEAP`** 包裹；**`rt_cputime_sleep`** 在栈上使用 **`struct rt_cputimer`** 并调用 **`rt_cputimer_init`**，与头文件条件编译**不完全一致**，应用层以 **`cputimer.c` 实际链接符号** 为准。

---

## 7. 使用与扩展建议

1. **仅测时间间隔**：在 **`INIT_BOARD`** 阶段由 **`cputime_*_init`** 注册 ops 后，使用 **`t1 = clock_cpu_gettime()`** … **`clock_cpu_millisecond(t2-t1)`** 即可。
2. **需要 `rt_cputimer` / 高精度 sleep**：必须提供 **`cputime_settimeout`**，并在绝对 tick 到达时调用框架期望的回调（与 **`cputimer.c`** 中 **`clock_cpu_settimeout(tick, _cputime_timeout_callback, t)`** 语义一致），或改造 **`cputimer.c`** 用其它定时源。
3. **RISC-V**：务必配置 **`CPUTIME_TIMER_FREQ`** 与 **`rdtime`** 实际频率一致。

---

## 8. 小结

| 项目 | 说明 |
|------|------|
| 目录职责 | **CPU 高精度 tick** + **可选硬件单次超时** + **多实例软件定时器链表** + **sleep/delay** |
| 默认 Arch 后端 | Cortex-M（DWT 或 perf_counter）、RISC-V（**`rdtime`**）均 **只实现计数与分辨率** |
| **`cputimer`** | 强依赖 **`cputime_settimeout`**；默认 arch 后端下 **不可用**，需 BSP 扩展 |
| 阅读顺序 | **`cputime.h`** → **`cputime.c`** → 对应 **`cputime_*arch*.c`** → **`cputimer.c`** |

按上述边界使用，可避免在未实现 **`settimeout`** 的平台上误用 **`rt_cputimer_*`** 导致断言失败或逻辑无效。
