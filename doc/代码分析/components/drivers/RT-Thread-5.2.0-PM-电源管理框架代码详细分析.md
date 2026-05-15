# RT-Thread 5.2.0 PM（电源管理）驱动框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/pm` 目录实现的 **系统级电源管理框架**：在 **idle 线程**中调用 **`rt_system_power_manager`**，根据各模式的 **请求计数**、**按模块位图表达的睡眠偏好**、**模块忙等待** 以及可选的 **tickless 定时唤醒**，选择 **睡眠深度** 并回调板级 **`rt_pm_ops`**；同时提供 **运行模式（调频）**、**带 PM 能力的设备 suspend/resume 链表**、以及 **`struct rt_timer` 封装的软低功耗定时器（lptimer）** 辅助 tickless。对外接口在 **`components/drivers/include/drivers/pm.h`** 与 **`drivers/lptimer.h`**；**`rtdevice.h`** 在 **`RT_USING_PM`** 下包含 **`drivers/pm.h`**。

---

## 1. 目录与编译

| 文件 | 作用 |
|------|------|
| **`pm.c`** | **`struct rt_pm` 单例**、**`rt_system_pm_init`**、**`rt_system_power_manager`**、请求/释放 API、设备注册、**Finsh/MSH** 调试命令 |
| **`lptimer.c`** | **软 lptimer** 链表、**`rt_lptimer_*`**、**`rt_lptimer_next_timeout_tick`**（供 **`pm_timer_next_timeout_tick`** 使用） |
| **`Kconfig`** | **`RT_USING_PM`** 及 **tickless 阈值**、**自定义模块 ID**、**调试**、**设备 suspend 失败回退**、**按阈值降级睡眠深度** 等 |
| **`SConscript`** | 仅在 **`RT_USING_PM`** 时编译 **`pm.c`** 与 **`lptimer.c`** |

---

## 2. Kconfig 选项摘要

| 选项 | 含义 |
|------|------|
| **`RT_USING_PM`** | 总开关（默认 **n**） |
| **`PM_TICKLESS_THRESHOLD_TIME`** | 未开启 **`PM_ENABLE_THRESHOLD_SLEEP_MODE`** 时，**`pm_get_sleep_threshold_mode`** 中若 **`timeout_tick`** 小于该值则将睡眠模式 **降为 `PM_SLEEP_MODE_IDLE`**（默认 **2** tick） |
| **`PM_USING_CUSTOM_CONFIG`** | 由 **`pm_cfg.h`** 提供 **`enum pm_module_id`**，替代默认 **`PM_*_ID`** 列表 |
| **`PM_ENABLE_DEBUG`** | 编译 **`pm_sleep_dump`**、**`pm_sleep_request`/`release` MSH** 等调试入口 |
| **`PM_ENABLE_SUSPEND_SLEEP_MODE`** | **`_pm_device_suspend`** 失败时 **resume 已挂起设备**、通知 **EXIT**、并将 **`sleep_mode` 限制在 `PM_SUSPEND_SLEEP_MODE` 以上** 再进入 **`ops->sleep`** |
| **`PM_ENABLE_THRESHOLD_SLEEP_MODE`** | 启用 **`PM_LIGHT/DEEP/STANDBY_THRESHOLD_TIME`**，按 **剩余睡眠时间** 在 **Idle / Light / Deep / Standby** 间降级 |

---

## 3. 模式与模块（`pm.h`）

### 3.1 睡眠模式 **`PM_SLEEP_MODE_*`**

从 **`PM_SLEEP_MODE_NONE`** 到 **`PM_SLEEP_MODE_SHUTDOWN`**，数值 **越大表示可进入越深（或越彻底）的睡眠语义**，具体行为由 **`rt_pm_ops::sleep`** 板级实现定义。

### 3.2 运行模式 **`PM_RUN_MODE_*`**

**`PM_RUN_MODE_HIGH_SPEED`** … **`PM_RUN_MODE_LOW_SPEED`**，配合 **`rt_pm_ops::run`** 与 **`struct rt_device_pm_ops::frequency_change`** 做 **调频/换时钟**。

### 3.3 模块 ID

默认 **`enum pm_module_id`**（**`PM_NONE_ID`** … **`PM_TP_ID`** 等）；**`PM_USING_CUSTOM_CONFIG`** 时由 **`pm_cfg.h`** 替换。**`PM_MODULE_MAX_ID`** 为枚举上界，用于 **数组维度** 与 **`sleep_status` 位图**。

### 3.4 设备控制命令

- **`RT_PM_DEVICE_CTRL_RELEASE` / `RT_PM_DEVICE_CTRL_REQUEST`**：对 **`/dev` 风格 `"pm"` 设备** 的 **`control`** 封装 **`rt_pm_release`/`rt_pm_request`**。

### 3.5 核心对象

**`struct rt_pm`**（节选）：

- **`modes[PM_SLEEP_MODE_MAX]`**：各睡眠模式的 **引用计数**（**`rt_pm_request`/`release`**）。
- **`sleep_mode` / `run_mode`**：当前状态。
- **`module_status[PM_MODULE_MAX_ID]`**：**`req_status`**、**`busy_flag`**、**`timeout`/`start_time`**（**`rt_pm_module_delay_sleep`**）。
- **`sleep_status[PM_SLEEP_MODE_MAX - 1][(PM_MODULE_MAX_ID + 31) / 32]`**：**按模式、按模块位** 的 **“该模块希望系统至少能进到的最深相关模式”** 类请求（与 **`modes[]` 计数** 为两套机制）。
- **`device_list`**：**`rt_slist_t`** 链接 **`struct rt_device_pm`**（suspend/resume/frequency_change）。
- **`timer_mask`**：每一位对应一种睡眠模式是否支持 **tickless 硬件/板级定时唤醒**（**`rt_system_pm_init` 参数传入**）。
- **`flags`**：如 **`RT_PM_FREQUENCY_PENDING`**（**`rt_pm_run_enter`** 在升频路径上 **延迟到 idle** 再应用）。

**`struct rt_pm_ops`**：**`sleep`/`run`/`timer_start`/`timer_stop`/`timer_get_tick`** — 全由 BSP 实现（可为空指针，但 **`rt_system_power_manager`** 在 **`_pm_change_sleep_mode`** 里对 **非 NONE** 路径会调用 **`pm_sleep`**，即 **`ops->sleep`**）。

**`struct rt_device_pm_ops`**：**`suspend`/`resume`/`frequency_change`**。

---

## 4. `pm.c`：电源管理主状态机

### 4.1 进入/退出临界区（可覆盖）

```68:76:rt-thread-5.2.0/components/drivers/pm/pm.c
rt_weak rt_uint32_t rt_pm_enter_critical(rt_uint8_t sleep_mode)
{
    return rt_hw_interrupt_disable();
}

rt_weak void rt_pm_exit_critical(rt_uint32_t ctx, rt_uint8_t sleep_mode)
{
    rt_hw_interrupt_enable(ctx);
}
```

SoC 可在深度睡眠前改用 **PRIMASK 以外** 的锁或 **电源域锁**。

### 4.2 睡眠模式选择 **`_pm_select_sleep_mode`**

1. 初值 **`mode = _pm_default_deepsleep`**（默认 **`RT_PM_DEFAULT_DEEPSLEEP_MODE`**，一般为 **`PM_SLEEP_MODE_DEEP`**）。
2. **`request_mode = _judge_sleep_mode()`**：扫描 **`sleep_status`** 二维位图，返回 **最小的非空模式索引**（**浅睡眠优先**）；若全空则返回 **`PM_SLEEP_MODE_MAX`**。
3. 再扫描 **`pm->modes[index]`**，从 **`PM_SLEEP_MODE_NONE`** 向上找 **第一个计数非 0** 的模式作为 **`mode`**（**`rt_pm_request` 计数约束**）。
4. 若 **`request_mode < mode`**，则 **`mode = request_mode`** — 取 **更浅** 的一侧，保证 **“有模块声明只能浅睡”** 时不会进太深。

### 4.3 模块忙 **`rt_pm_module_delay_sleep`**

设置 **`busy_flag`** 与 **`timeout`**、**`start_time`**；**`_pm_device_check_idle`** 在超时后自动清除 **`busy_flag`**。若任一模块仍 **busy**，**`_pm_change_sleep_mode`** 将睡眠限制在 **`PM_BUSY_SLEEP_MODE`**（默认 **`PM_SLEEP_MODE_IDLE`**），且若该模式 **浅于** 当前选中模式则 **再压低 `sleep_mode`**。

### 4.4 状态迁移 **`_pm_change_sleep_mode`**

概要流程：

1. **`rt_pm_enter_critical`**。
2. **`pm->sleep_mode = _pm_select_sleep_mode(pm)`**，再经 **busy** 修正。
3. **`PM_SLEEP_MODE_NONE`**：仅 **`pm_sleep(pm, NONE)`** 后退出临界区（**不断电的“空转”语义由 BSP 定义**）。
4. 否则：
   - **`rt_pm_notify_set`**：**`RT_PM_ENTER_SLEEP`**。
   - **`_pm_device_suspend`**（可选 **`PM_ENABLE_SUSPEND_SLEEP_MODE`** 失败回退分支）。
   - **Tickless**：若 **`timer_mask` 含当前 `sleep_mode`**：
     - **`timeout_tick = pm_timer_next_timeout_tick(sleep_mode)`** 相对当前 tick 的差值；
     - **`pm_get_sleep_threshold_mode`** 可能 **因剩余时间过短而降级模式**；
     - 若降级后模式仍带 timer 位，**`pm_lptimer_start(pm, timeout_tick)`**（板级 **`timer_start`**）。
   - **`pm_sleep(pm, sleep_mode)`** — **实际 WFI/关时钟/掉电** 等。
   - 唤醒后：若使用了 tickless，**`pm_lptimer_get_timeout`** 与 **`rt_tick_increase_tick(delta_tick)`** 补偿系统 tick。
   - **`_pm_device_resume`** → **`RT_PM_EXIT_SLEEP` 通知** → **`rt_pm_exit_critical`**。

### 4.5 **`rt_system_power_manager`**

```468:480:rt-thread-5.2.0/components/drivers/pm/pm.c
void rt_system_power_manager(void)
{
    if (_pm_init_flag == 0 || _pm.ops == RT_NULL)
    {
        return;
    }

    /* CPU frequency scaling according to the runing mode settings */
    _pm_frequency_scaling(&_pm);

    /* Low Power Mode Processing */
    _pm_change_sleep_mode(&_pm);
}
```

由 **内核 idle** 每轮调用（见 **`src/idle.c`** 中 **`#ifdef RT_USING_PM`**）。因此 **PM 策略与 idle 调度强绑定**；**`IDLE_THREAD_STACK_SIZE` 必须大于 256**（**`rt_system_pm_init`** 内 **`#error`** 保护）。

### 4.6 请求/释放 API（计数与位图）

| API | 作用 |
|-----|------|
| **`rt_pm_request` / `rt_pm_release` / `rt_pm_release_all`** | 对 **`pm->modes[mode]`** 做 **0..255** 饱和增减或清零 |
| **`rt_pm_module_request` / `release` / `release_all`** | 在 **`module_status[module_id].req_status`** 与 **`modes[]`** 上组合更新（**`release` 在计数到 0 时清 `req_status`**） |
| **`rt_pm_sleep_request` / `rt_pm_sleep_release`** 及 **`rt_pm_sleep_*_request/release` 便捷封装** | 设置/清除 **`sleep_status[mode][word]`** 中 **对应 module_id 的 bit** |

两套机制并存：**`modes[]`** 偏 **“有多少客户端要求系统能睡到某档”**；**`sleep_status`** 偏 **“每个模块对某睡眠档的显式绑定”** — 移植时需对照 BSP 文档选用，避免 **重复 request 导致无法深睡**。

### 4.7 运行模式 **`rt_pm_run_enter`**

- **`mode < pm->run_mode`**（数值更小 = 更高性能档）：立即 **`ops->run`** + **`_pm_device_frequency_change`**。
- 否则（**降性能/省电运行档**）：仅置 **`RT_PM_FREQUENCY_PENDING`**，在下次 idle **`_pm_frequency_scaling`** 再应用，避免在 **非 idle 上下文** 切时钟。

### 4.8 设备注册

- **`rt_pm_device_register`**：**`RT_KERNEL_MALLOC`** **`struct rt_device_pm`**，**`rt_slist_append`** 到 **`device_list`**。
- **`rt_pm_device_unregister`**：按 **`device` 指针** 查找并 **`rt_slist_remove`** + **`RT_KERNEL_FREE`**。

**顺序**：**suspend** 按 **链表顺序** 调用；**resume** 亦为 **同序遍历**（非栈式逆序），若硬件依赖 **特定 suspend/resume 顺序**，需在 BSP **注册顺序** 上自行保证。

### 4.9 **`rt_system_pm_init`**

- 注册 **`RT_Device_Class_PM`** 设备 **`"pm"`**，挂 **`read`/`write`/`control`** 或 **`rt_device_ops`**。
- **`pm->modes`** 清零后 **`pm->modes[pm->sleep_mode] = 1`**（**`_pm_default_sleep`**），**`PM_POWER_ID` 的 `req_status = 1`**。
- **`run_mode = RT_PM_DEFAULT_RUN_MODE`**，保存 **`timer_mask`** 与 **`ops`**，初始化 **`device_list`**，**`_pm_init_flag = 1`**。

### 4.10 弱符号 **`pm_timer_next_timeout_tick` / `pm_get_sleep_threshold_mode`**

- **`pm_timer_next_timeout_tick(mode)`**：**`LIGHT`** 用 **`rt_timer_next_timeout_tick()`**（内核软件定时器）；**`DEEP`/`STANDBY`** 用 **`rt_lptimer_next_timeout_tick()`**；否则 **`RT_TICK_MAX`**。BSP 可 **weak 覆盖** 以接 **RTC/低功耗时基**。
- **`pm_get_sleep_threshold_mode`**：按 **`PM_ENABLE_THRESHOLD_SLEEP_MODE`** 与 **Kconfig 阈值** 或 **`PM_TICKLESS_THRESHOLD_TIME`** 做 **模式降级**。

### 4.11 Finsh / MSH

**`RT_USING_FINSH`** 下：**`pm_request`/`pm_release`/`pm_release_all`/`pm_module_*`/`pm_module_delay`/`pm_run`/`pm_dump`** 等；**`pm_dump`** 同时 **`FINSH_FUNCTION_EXPORT_ALIAS`** 与 **`MSH_CMD_EXPORT_ALIAS`**。**`PM_ENABLE_DEBUG`** 下额外 **`pm_sleep_dump`** 等。

---

## 5. `lptimer.c`：软低功耗定时器

### 5.1 数据结构

**`struct rt_lptimer`**（**`lptimer.h`**）：内嵌 **`struct rt_timer timer`** + **`rt_list_t list`**。

### 5.2 与内核 **`rt_timer` 的关系

**`rt_lptimer_init`** 直接 **`rt_timer_init`**；**`start`/`stop`/`detach`/`control`** 委托给 **`rt_timer_*`**。

### 5.3 **`rt_soft_lptimer_list`**

**`rt_lptimer_start`** 在 **关中断** 下：先从链表摘除，**`rt_timer_start`** 成功后 **插入全局 `rt_soft_lptimer_list`**。该链表 **仅用于遍历/求下一超时**，不改变 **`rt_timer`** 内核管理逻辑。

### 5.4 **`rt_lptimer_next_timeout_tick`**

遍历链表中 **已激活**（**`RT_TIMER_FLAG_ACTIVATED`**）的项，取 **最小的 `timeout_tick`** 返回；空链表返回 **`RT_TICK_MAX`**。

### 5.5 **`lptimer_dump`**

MSH 打印各 **lptimer** 的 **周期、timeout_tick、激活标志**。

**用途**：在 **深睡前** 估算 **“多久后必须醒来处理软定时器任务”**，与 **`rt_pm_ops::timer_start`** 配合做 **硬件低功耗定时器编程**。

---

## 6. 与内核 idle 的衔接

```173:177:rt-thread-5.2.0/src/idle.c
#ifdef RT_USING_PM
        void rt_system_power_manager(void);
        /* 电源管理模块：可在空闲时进入更低功耗状态 */
        rt_system_power_manager();
#endif /* RT_USING_PM */
```

**要点**：**SMP** 下每个 CPU 均有 idle，**`rt_system_power_manager`** 无 CPU 索引参数，**多核协同休眠** 需在 **`rt_pm_ops::sleep` 或 weak 临界区** 中由 BSP 处理。

---

## 7. BSP 集成要点

1. 实现 **`struct rt_pm_ops`**：**`sleep`** 必选语义清晰；**`run`** 用于调频；**`timer_*`** 用于 **tickless 补偿**。
2. 上电路径调用 **`rt_system_pm_init(ops, timer_mask, user_data)`**，**`timer_mask`** 按位指明 **哪些 `PM_SLEEP_MODE_*` 在进睡前后需要启动/停止板级定时器**。
3. 需要休眠外设时，在驱动 **`probe`** 中 **`rt_pm_device_register(dev, &pm_ops)`**，实现 **`suspend`/`resume`**。
4. 应用层通过 **`rt_pm_request(PM_SLEEP_MODE_DEEP)`** 等 **提高可进入深度**；**`rt_pm_release`** 配对使用。
5. **`pm_cfg.h` + `PM_USING_CUSTOM_CONFIG`**：扩展 **`PM_MODULE_MAX_ID`** 时注意 **`sleep_status` 第二维大小** 与 **`module_id`** 范围一致。

---

## 8. 小结

| 组件 | 职责 |
|------|------|
| **`pm.h`** | 模式枚举、**`rt_pm`/`rt_pm_ops`/`rt_device_pm_ops`**、API 声明 |
| **`pm.c`** | **idle 驱动**的状态机、**tickless** 钩子、**设备链表**、**MSH 调试** |
| **`lptimer.c`** | **基于 `rt_timer` 的软 lptimer 列表** 与 **下一唤醒 tick** 查询 |

该框架 **不实现具体 MCU 睡眠指令**，只定义 **策略与调用顺序**；**功耗效果取决于 `rt_pm_ops` 与设备 `suspend` 实现质量**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/pm/`。*
