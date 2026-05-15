# RT-Thread 5.2.0 RTC（实时时钟）设备框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/rtc` 目录实现的 **RTC 设备框架 V2.0**：以 **`rt_rtc_dev_t`** 注册名为 **`"rtc"`** 的字符设备，通过 **`rt_device_control`** 与 **`struct rt_rtc_ops`** 对接硬件；提供 **`set_date`/`set_time`/`set_timestamp`/`get_timestamp`** 等 libc 友好封装；可选 **Alarm 服务线程** 与 **软件 RTC（tick/ktime 推导时间）**。对外接口在 **`components/drivers/include/drivers/dev_rtc.h`**；**`RT_USING_ALARM`** 时 **`rtdevice.h`** 额外包含 **`drivers/dev_alarm.h`**。

---

## 1. 目录与编译

| 文件 | 条件 | 作用 |
|------|------|------|
| **`dev_rtc.c`** | **`RT_USING_RTC`** | **`rt_hw_rtc_register`**、**`rt_rtc_control`**、**时间设置/读取 API**、**`date` MSH** |
| **`dev_alarm.c`** | **`RT_USING_RTC` && `RT_USING_ALARM`** | **闹钟容器、服务线程、`rt_alarm_*` API** |
| **`dev_soft_rtc.c`** | **`RT_USING_RTC` && `RT_USING_SOFT_RTC`** | **软件 `"rtc"` 设备**、可选 **ktime 高精度**、**`rt_soft_rtc_sync`**（**`RT_USING_SYSTEM_WORKQUEUE`**） |
| **`README.md`** | — | 使用说明（与代码一致强调 **全局仅一个 `"rtc"`**） |
| **`Kconfig` / `SConscript`** | — | 选项与源文件编排 |

**`SConscript`**：在 **`RT_USING_RTC`** 下始终编译 **`dev_rtc.c`**；**`RT_USING_ALARM`** 增加 **`dev_alarm.c`**；**`RT_USING_SOFT_RTC`** 增加 **`dev_soft_rtc.c`**。

---

## 2. Kconfig

| 选项 | 含义 |
|------|------|
| **`RT_USING_RTC`** | 总开关（默认 **n**） |
| **`RT_USING_ALARM`** | 编译 **`dev_alarm.h`/`dev_alarm.c`**；可配置 **`RT_ALARM_STACK_SIZE`/`TIMESLICE`/`PRIORITY`** |
| **`RT_USING_SOFT_RTC`** | 软件模拟 RTC（**`INIT_DEVICE_EXPORT`** 注册 **`"rtc"`**） |

---

## 3. 设备模型与 control 命令（`dev_rtc.h`）

### 3.1 `struct rt_rtc_ops`

| 成员 | 典型用途 |
|------|----------|
| **`init`** | **`rt_device_init` 时调用**（可为空则 **`-RT_ENOSYS`**）。 |
| **`get_secs` / `set_secs`** | **`time_t` 秒级读写**（UTC 语义由驱动约定）。 |
| **`get_alarm` / `set_alarm`** | **`struct rt_rtc_wkalarm`** 硬件闹钟。 |
| **`get_timeval` / `set_timeval`** | **微秒级**（配合 **`gettimeofday`**）。 |

### 3.2 `struct rt_rtc_wkalarm`

**`enable`** 与 **`tm_*`** 字段（**`tm_year` 等与 `struct tm` 一致：月 0–11 等**），由 **`dev_alarm.c`** 填充后下发 **`RT_DEVICE_CTRL_RTC_SET_ALARM`**。

### 3.3 `rt_rtc_dev_t`

**`struct rt_device parent` + `const struct rt_rtc_ops *ops`**。

### 3.4 Control 宏（节选）

**`RT_DEVICE_CTRL_RTC_GET/SET_TIME`**（秒）、**`GET/SET_TIMEVAL`**、**`GET/SET_ALARM`**；另有 **`GET/SET_TIMESPEC`**、**`GET_TIMERES`**（**`clock_gettime`/`clock_getres`** 用，见 **`components/libc/compilers/common/ctime.c`**）。

**注意**：**`dev_rtc.c`** 中 **`rt_rtc_control`** **仅分发** 到 **`GET/SET_TIME`、`TIMEVAL`、`ALARM`**；**`TIMESPEC`/`TIMERES`** 需 **具体 RTC 设备** 在 **`control` 路径** 中实现（**`dev_soft_rtc.c`** 在 **`soft_rtc_control`** 中实现；硬件 BSP 驱动若走 **`rt_hw_rtc_register`**，需在 **`rt_rtc_ops` 之外** 扩展 **`rt_device` 的 `control`** 或通过 **包装设备** 支持——常规做法是 **BSP 将 `rt_rtc_dev_t` 嵌入更大结构并重写 `parent.control`**，或 **libc 直接 `rt_device_find("rtc")` 后 `control`**）。

---

## 4. `dev_rtc.c`：核心实现

### 4.1 `rt_hw_rtc_register`

- 设备类型 **`RT_Device_Class_RTC`**。
- **`RT_USING_DEVICE_OPS`** 时挂 **`rtc_core_ops`**：**`init`/`open`/`close`/`control`**。
- **`rt_device_register(device, name, flag)`** — 名称一般由 BSP 定为 **`"rtc"`**。

### 4.2 `rt_rtc_control`

**`TRY_DO_RTC_FUNC`** 宏：若 **`ops->对应函数`** 非空则调用，否则 **`-RT_EINVAL`**。**`default` 分支** 对未知 cmd 仍返回 **初始化的 `-RT_EINVAL`**。

### 4.3 全局缓存 **`_rtc_device`**

**`set_date`/`set_time`/`set_timestamp`/`get_timestamp`** 首次调用 **`rt_device_find("rtc")`** 并缓存指针；**未 `open` 即 `control`** — 依赖 **`rt_device_control`** 在驱动侧是否要求 **已打开**（多数 BSP **允许未 open**）。

### 4.4 `set_date` / `set_time`

1. **`GET_TIME`** 取当前 **`time_t`**。
2. **`localtime_r`** 拆成 **`struct tm`**。
3. 只改 **日期或时分秒**，**`mktime`** 回 **`time_t`**（**本地时区**）。
4. **`SET_TIME`** 写回。

与 **`set_timestamp`**（直接 **`SET_TIME`**）区分：**前者按本地日历语义改字段，后者整秒 UTC 写入**（与注释一致）。

### 4.5 Finsh **`date` 命令**

- **无参**：**`gettimeofday`** 打印 **local time**、**timestamps**、可选 **`rt_tz_get()`** 时区偏移（**`RT_LIBC_USING_LIGHT_TZ_DST`**）。
- **7 参数**：解析 **`struct tm`**，**`get_timestamp` → `mktime` → `set_timestamp`**，打印变更前后 **ctime**。

---

## 5. `dev_alarm.c`：闹钟子系统

### 5.1 依赖

- 通过 **`rt_device_find("rtc")`** 访问 RTC；**`alarm_set`** 使用 **`RT_DEVICE_CTRL_RTC_SET/GET_ALARM`**。
- 与 **`get_timestamp`/`gmtime_r`** 结合做 **纯软件匹配**（周期性闹钟在 **`alarm_wakeup`** 里 **更新 `wktime`** 并 **调 `callback`**）。

### 5.2 容器 **`struct rt_alarm_container`**

**`head` 链表**、**`mutex`**、**`event`**（**服务线程阻塞接收**）、**`current`**（**当前硬件已编程的那条 alarm**）。

### 5.3 线程 **`alarmsvc`**

**`INIT_PREV_EXPORT(rt_alarm_system_init)`** 创建线程，循环 **`rt_event_recv`** 后 **`alarm_update`**。

### 5.4 `rt_alarm_update`（BSP 调用）

**`rt_event_send(&_container.event, 1)`** — 硬件 RTC **闹钟中断**里应调用 **`rt_alarm_update(dev, event)`** 唤醒服务线程，再 **`alarm_update`** 内：

- 遍历所有 **`rt_alarm`**：**`alarm_wakeup`**（**秒/分/时/单次/日/周/月/年** 等模式，见 **`RT_ALARM_*` 标志**）。
- 再按 **当日秒数** 选择 **下一个** **`alm_next`** 或跨天 **`alm_prev`**，对 **`current`** 调用 **`alarm_set`** 写硬件。

### 5.5 **`RT_ALARM_DELAY`**

未定义 **`RT_USING_SOFT_RTC`** 时为 **2 秒** 容差；**软 RTC** 下为 **0**（匹配 **1 秒软定时器** 粒度）。

### 5.6 对外 API（`dev_alarm.h`）

**`rt_alarm_create`/`start`/`stop`/`delete`/`control`**；**`rt_alarm_dump`** → **MSH `list_alarm`**。

**拼写**：**`RT_ALARM_YAERLY`** 为历史宏名（**yearly**）。

---

## 6. `dev_soft_rtc.c`：软件 RTC

### 6.1 时间基准

- **`init_tick`**（**`rt_tick_get()`**）+ **`init_time`**（**`timegm(SOFT_RTC_TIME_DEFAULT)`**）。
- **`GET_TIME`**：**`init_time + (rt_tick_get() - init_tick) / RT_TICK_PER_SECOND`**。
- **`set_rtc_time`**：反解 **`init_time`**，使 **当前 tick 对应新绝对时间**；**`RT_USING_ALARM`** 时刷新 **软闹钟定时器**。

### 6.2 与硬件 RTC 互斥

**`RT_ASSERT(!rt_device_find("rtc"))`** 在 **`INIT_DEVICE_EXPORT`** 前执行——**系统中只能存在一个 `"rtc"`**；若 **`BSP_USING_ONCHIP_RTC`** 与 **`RT_USING_SOFT_RTC`** 同时定义会 **编译 `#warning`**。

### 6.3 高精度路径 **`RT_USING_KTIME`**

**`GET/SET_TIMEVAL`、`GET/SET_TIMESPEC`、`GET_TIMERES`** 使用 **`rt_ktime_boottime_*`** 与 **`init_tv`/`init_ts`** 保存 **亚秒偏移**。

### 6.4 软 RTC + Alarm

**`rt_timer`** 周期 **1 秒**（**`RT_TIMER_FLAG_SOFT_TIMER|ONE_SHOT`**），超时 **`alarm_timeout` → `rt_alarm_update`**，模拟 **硬件秒中断**。

### 6.5 **`RT_USING_SYSTEM_WORKQUEUE`**

**`rt_soft_rtc_sync`**：从 **`soft_rtc_dev`** 读时间并 **`set_rtc_time`**（**校准 tick 基准**）。**`rt_soft_rtc_set_source`**：向 **workqueue** 提交周期性任务，从 **名为 `name` 的另一设备** 同步（BSP 需提供可读时间源）；**`rtc_sync` MSH**。

---

## 7. 与 libc 时间的关系

**`ctime.c`** 通过 **`_control_rtc`** 对 **`"rtc"`** 下发 **`TIMESPEC`/`TIMERES`** 等；若 **仅硬件 `rt_rtc_ops`** 且 **无 `TIMESPEC` 实现**，需在 **驱动层** 补齐 **`control`** 或 **使用软 RTC + ktime**。

---

## 8. 小结

| 组件 | 职责 |
|------|------|
| **`dev_rtc.h/c`** | **标准 RTC 设备 ops + 便捷 API + `date` 命令** |
| **`dev_alarm.h/c`** | **多模式闹钟 + 服务线程 + 硬件编程调度** |
| **`dev_soft_rtc.c`** | **无硬件 RTC 时的 tick/ktime 模拟 + 可选同步** |

BSP 集成要点：**实现 `rt_rtc_ops`**，**`rt_hw_rtc_register(&rtc_dev, "rtc", ...)`**；若用 Alarm，**闹钟 ISR 中调用 `rt_alarm_update`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/rtc/`。*
