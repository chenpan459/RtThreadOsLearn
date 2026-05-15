# RT-Thread 5.2.0 HWTIMER 硬件定时器框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/hwtimer` 目录下的代码职责：**通用硬件定时器字符设备框架**（`hwtimer.c`）与可选的 **ARM Architected Timer 平台驱动片段**（`hwtimer-arm_arch.c`）。对外类型与命令定义在 **`components/drivers/include/drivers/hwtimer.h`**，`rtdevice.h` 在 **`RT_USING_HWTIMER`** 下包含该头文件。

涉及文件：

- 框架实现：`hwtimer.c`
- ARM 体系结构定时器（依赖 DM）：`hwtimer-arm_arch.c`
- 构建：`Kconfig`、`SConscript`

---

## 1. 模块定位与分层

```text
应用 / 组件（write/read/ioctl 语义）
    ↓
rt_device（类型 RT_Device_Class_Timer）
    ↓
hwtimer.c：timeout 分解、overflow/cycles 软件计数、read 换算时间
    ↓
rt_hwtimer_ops：init / start / stop / count_get / control
    ↓
BSP：片上 GPT/Timer 等硬件 + 中断里调用 rt_device_hwtimer_isr()
```

**与 `hwtimer-arm_arch.c` 的关系**：后者在 **`RT_HWTIMER_ARM_ARCH`** 开启时编译，通过 **设备树 `compatible`** 匹配 **`arm,armv7-timer` / `arm,armv8-timer`**，安装中断并在 ISR 中调用 **`rt_tick_increase()`**，属于 **系统节拍（tick）** 路径；**不**通过 **`rt_device_hwtimer_register`** 暴露为 **`rt_hwtimer_t`** 设备。二者文件名同属 `hwtimer` 目录，但 **用户态“硬件定时器设备”** 与 **内核 tick 源** 是两条线，阅读时勿混淆。

---

## 2. Kconfig 与编译

| 选项 | 含义 |
|------|------|
| **`RT_USING_HWTIMER`** | 总开关；关闭时 `SConscript` 直接 `Return`，不编译本组 |
| **`RT_HWTIMER_ARM_ARCH`** | 编译 **`hwtimer-arm_arch.c`**；依赖 **`RT_USING_DM`**、**`RT_USING_HWTIMER`**，且 SoC 为 **`ARCH_ARM_CORTEX_A` 或 `ARCH_ARMV8`** |

**`SConscript`**：默认只编 **`hwtimer.c`**；**`CPPPATH`** 为 **`../include`**（与其它 DeviceDrivers 组一致）。

---

## 3. 关键数据结构（`hwtimer.h`）

### 3.1 `struct rt_hwtimer_ops`（前向声明为 `struct rt_hwtimer_device`）

| 成员 | 职责 |
|------|------|
| **`init(timer, state)`** | **`state==1`**：框架侧完成默认频率/模式后的硬件初始化；**`state==0`**：`close` 时反初始化 |
| **`start(timer, cnt, mode)`** | 以当前 **`freq`** 为时钟，装载计数值 **`cnt`**，按 **`HWTIMER_MODE_ONESHOT` / `PERIOD`** 启动 |
| **`stop`** | 停止计数 |
| **`count_get`** | 返回当前计数值（与 **`info->cntmode`** 配合，见 **`read`**） |
| **`control`** | 处理 **`HWTIMER_CTRL_FREQ_SET`**（框架会先校验频率范围，成功后再写 **`timer->freq`**）；未识别的 **`cmd`** 也会落到此处 |

### 3.2 `struct rt_hwtimer_info`

- **`maxfreq` / `minfreq`**：驱动支持的计数时钟范围。
- **`maxcnt`**：单次计数溢出前的最大值（与 **`timeout_calc`** 中“单次最长定时”相关）。
- **`cntmode`**：**`HWTIMER_CNTMODE_UP`** 或 **`HWTIMER_CNTMODE_DW`**。

### 3.3 `typedef struct rt_hwtimer_device { ... } rt_hwtimer_t`

- **`parent`**：`struct rt_device`。
- **`ops` / `info`**：只读指针，**`rt_device_hwtimer_register`** 时 **`RT_ASSERT`** 非空。
- **`freq`**：当前计数频率；**`init`** 中若 **[minfreq, maxfreq]** 含 **1 MHz** 则默认 **1 MHz**，否则取 **`minfreq`**。
- **`overflow`**：在 **`rt_device_hwtimer_isr`** 中自增，表示硬件计数溢出次数。
- **`period_sec`**：**`timeout_calc`** 算出的“单次溢出周期”（秒，浮点）。
- **`cycles` / `reload`**：还需多少次溢出才触发一次“超时完成”；周期模式下 **`cycles` 用完后重置为 `reload`**。
- **`mode`**：**单次**或**周期**（亦可由 **`write`** 根据 **`cycles`** 临时覆盖为单次）。

### 3.4 `rt_hwtimerval_t`

**`sec` + `usec`**：`write` 的定时间隔；`read` 返回自启动以来（结合 overflow）换算的已流逝时间。

### 3.5 `rt_hwtimer_ctrl_t`

- **`HWTIMER_CTRL_FREQ_SET`**：设置频率（参数为 **`rt_int32_t*`**）。
- **`HWTIMER_CTRL_STOP`**：停止。
- **`HWTIMER_CTRL_INFO_GET`**：拷贝 **`*timer->info`** 到 **`args`**。
- **`HWTIMER_CTRL_MODE_SET`**：设置 **`HWTIMER_MODE_ONESHOT` / `PERIOD`**。

---

## 4. 框架实现要点（`hwtimer.c`）

### 4.1 `timeout_calc`

将 **`rt_hwtimerval_t`** 转为单次硬件装载值 **`counter`**，并填写 **`cycles`、`reload`、`period_sec`**：

- 用 **`info->maxcnt` 与 `freq`** 得到单次最大定时 **`overflow`**（秒）。
- 若目标时间小于 **一个计数 tick**（**`1/freq`**），则退化为最短间隔。
- 否则在 **`for (i = 1; i > 0; i++)`** 中增大 **`i`**（表示“分 **`i`** 段溢出”），使每段 **`timeout = tv_sec/i`** 不超过 **`overflow`**，并以与目标时间的偏差 **`devi`** 选取较优的 **`i`**（实现上为启发式搜索，循环靠 **`break`** 退出）。

### 4.2 `rt_hwtimer_init` / `open` / `close`

- **`init`**：设默认 **`freq`、mode、cycles、overflow**，再 **`ops->init(timer, 1)`**；无 **`init`** 则 **`-RT_ENOSYS`**。
- **`open`**：**`ops->control(HWTIMER_CTRL_FREQ_SET, &timer->freq)`** 把初始化频率下发硬件。
- **`close`**：**`ops->init(timer, 0)`**，清 **`ACTIVATED`** 与 **`rx_indicate`**。

### 4.3 `read`

关中断读取 **`count_get`** 与 **`overflow`**。若 **`cntmode==DW`**，把递减计数换算成等价“已走格数”。单次模式下 **`overflow` 在 read 路径中视为 0**。最后换算为 **`rt_hwtimerval_t`** 拷贝到 **`buffer`**，长度取 **`min(size, sizeof(tv))`**。

### 4.4 `write`

要求 **`size == sizeof(rt_hwtimerval_t)`**。先 **`stop`**，清零 **`overflow`**，再 **`timeout_calc`** 得 **`t`**。若 **`cycles<=1` 且 `mode==ONESHOT`**，则 **`start`** 的 **`opm`** 为单次，否则为周期。调用 **`ops->start(timer, t, opm)`**。

### 4.5 `control`

见 3.5；**`FREQ_SET`** 超范围时打日志并保持原 **`freq`**，返回 **`-RT_EINVAL`**。

### 4.6 `rt_device_hwtimer_isr`

每次硬件溢出进入时：**`overflow++`**；若 **`cycles!=0`** 则 **`cycles--`**。当 **`cycles==0`**：恢复 **`cycles = reload`**；若为单次则 **`stop`**；若注册了 **`rx_indicate`**，则以 **`sizeof(struct rt_hwtimerval)`** 为“长度”回调（语义上表示一次定时完成事件）。

### 4.7 `rt_device_hwtimer_register`

设备类型 **`RT_Device_Class_Timer`**，标志 **`RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STANDALONE`**，挂接 **`rt_hwtimer_*`** 或 **`rt_device_ops`** 表。

### 4.8 `RT_USING_DM` 下的 `rt_hw_us_delay`

定义函数指针 **`rt_device_hwtimer_us_delay`** 与 **`rt_hw_us_delay`**：若指针非空则转调，否则 **`LOG_E`** 并 **`RT_ASSERT(0)`**。aarch64 等可在 **`libcpu`** 里把 **`rt_device_hwtimer_us_delay`** 指到 **`cpu_us_delay`**，与 **`kservice.c`** 中的 **`rt_weak rt_hw_us_delay`** 形成“强符号覆盖/并存”关系（具体链接顺序以工程为准）。**用途**：软 SPI/I2C、PCIe 等需要 **微秒级忙等** 时统一走 **`rt_hw_us_delay`**。

---

## 5. ARM Architected Timer（`hwtimer-arm_arch.c`）

### 5.1 作用

在 **DM（`RT_USING_DM`）** 与 **OFW 平台设备** 前提下，为 **Cortex-A / ARMv8** 注册 **`rt_platform_driver`**，在 **`probe`** 中：

- 按 **`ARCH_SUPPORT_TEE` / `ARCH_SUPPORT_HYP`** 与常规模型选择 **`mode_idx`**（**`ctrl_handle` / `value_handle`** 表项：Mon/Hyp/OS 的 physical/virtual timer）。
- 通过 **`rt_dm_dev_get_irq_by_name`** 或 **`rt_dm_dev_get_irq`** 取中断号，**`rt_hw_interrupt_install`** 安装 **`arm_arch_timer_isr`**。
- **`timer_step = arm_arch_timer_get_frequency() / RT_TICK_PER_SECOND`**，写 **TVAL** 并 **使能**，从而在 ISR 里周期性 **`rt_tick_increase()`**。

### 5.2 其它导出

- **`INIT_SECONDARY_CPU_EXPORT(arm_arch_timer_post_init)`**：从核上再次 **`arm_arch_timer_local_enable`**。
- **`arm_arch_timer_set_frequency`**：仅在 **`ARCH_SUPPORT_TEE`** 下写 **`CNTFRQ`**；否则 **`-RT_ENOSYS`**。

### 5.3 依赖头文件

**`cpu.h` / `cpuport.h`**：系统寄存器读写与 **`CNT*_*`** 符号，与 **libcpu** 紧耦合。

---

## 6. BSP 驱动作者注意事项

1. **`info`** 中 **`maxcnt`、`minfreq/maxfreq`、`cntmode`** 必须与硬件一致，否则 **`timeout_calc`** 与 **`read`** 换算会偏差。
2. **溢出中断** 中必须调用 **`rt_device_hwtimer_isr(timer)`**（参数为 **`rt_hwtimer_t*`**）。
3. **`start` 的 `cnt`**：框架已按 **`freq`** 换算好装载值；驱动应配置预分频/计数器使该值在硬件合法范围内。
4. **`count_get`**：在 **递减模式** 下，框架 **`read`** 会用 **`freq*period_sec - cnt`** 做等价转换，驱动需保证 **`count_get`** 与 **`start` 装载语义** 一致。
5. 若仅需 **tick** 且使用 **ARM 通用定时器 + DM**，可考虑 **`RT_HWTIMER_ARM_ARCH`**；若需 **多个用户可打开的定时器设备**，仍应对每个硬件通道 **`rt_device_hwtimer_register`**，与 **`arm_arch_timer`** 驱动无必然绑定。

---

## 7. 小结

| 项目 | 说明 |
|------|------|
| 核心文件 | **`hwtimer.c`**：通用 **`rt_hwtimer_t`** + **`rt_device_hwtimer_isr`** |
| 可选文件 | **`hwtimer-arm_arch.c`**：**tick** 用 ARM architected timer **平台驱动**，非通用 hwtimer 字符设备的唯一实现 |
| 对外 API | **`rt_device_hwtimer_register`**、**`rt_device_hwtimer_isr`**；控制命令见 **`hwtimer.h`** |
| DM 扩展 | **`rt_device_hwtimer_us_delay`** / **`rt_hw_us_delay`**（**`RT_USING_DM`**） |

阅读顺序：**`hwtimer.h`**（类型与命令）→ **`hwtimer.c`**（**`write`/`read`/ISR** 与 **`timeout_calc`**）→ 对照 BSP **`drv_hwtimer`**；若关心 **AArch64 tick**，再读 **`hwtimer-arm_arch.c`** 与 **`libcpu`** 中的寄存器定义。
