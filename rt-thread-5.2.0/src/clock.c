/*
 * Copyright (c) 2006-2024 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-12     Bernard      first version
 * 2006-05-27     Bernard      add support for same priority thread schedule
 * 2006-08-10     Bernard      remove the last rt_schedule in rt_tick_increase
 * 2010-03-08     Bernard      remove rt_passed_second
 * 2010-05-20     Bernard      fix the tick exceeds the maximum limits
 * 2010-07-13     Bernard      fix rt_tick_from_millisecond issue found by kuronca
 * 2011-06-26     Bernard      add rt_tick_set function.
 * 2018-11-22     Jesven       add per cpu tick
 * 2020-12-29     Meco Man     implement rt_tick_get_millisecond()
 * 2021-06-01     Meco Man     add critical section projection for rt_tick_increase()
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-10-16     RiceChen     fix: only the main core detection rt_timer_check(), in SMP mode
 */

/*
 * 本文件职责（中文概要）：
 * 维护系统节拍 tick（自系统上电/启动以来的时钟滴答计数），由硬件定时器中断里调用 rt_tick_increase*() 推进。
 * 每次 tick 会更新 CPU 占用统计（可选）、推进线程时间片 rt_sched_tick_increase()、并在主核上检查软件定时器 rt_timer_check()。
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtatomic.h>

#if defined(RT_USING_SMART) && defined(RT_USING_VDSO)
#include <vdso.h>
#endif

#ifdef RT_USING_SMP
/* SMP：逻辑上仍用「CPU0 的 tick」作为 rt_tick_get/rt_tick_set 的全局视图；各核另有 rt_cpu_self()->tick */
#define rt_tick rt_cpu_index(0)->tick
#else
/* 单核：全局原子变量，保证与 rt_tick_get 等并发读写的可见性 */
static volatile rt_atomic_t rt_tick = 0;
#endif /* RT_USING_SMP */

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_tick_hook)(void);

/**
 * @addtogroup group_Hook
 */

/**@{*/

/**
 * @brief This function will set a hook function, which will be invoked when tick increase
 *
 *
 * @param hook the hook function
 * @note 中文：每次 rt_tick_increase*() 推进 tick 时调用；须短小、不可阻塞。
 */
void rt_tick_sethook(void (*hook)(void))
{
    rt_tick_hook = hook;
}
/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_Clock
 */

/**@{*/

/**
 * @brief    This function will return current tick from operating system startup.
 *
 * @return   Return current tick.
 * @note     中文：返回自 OS 启动以来的 tick 数；线程与中断中均可读，底层为原子读。
 */
rt_tick_t rt_tick_get(void)
{
    /* return the global tick */
    return (rt_tick_t)rt_atomic_load(&(rt_tick));
}
RTM_EXPORT(rt_tick_get);

/**
 * @brief    This function will set current tick.
 *
 * @param    tick is the value that you will set.
 * @note     中文：强制设置全局 tick，一般仅调试或同步场景使用。
 */
void rt_tick_set(rt_tick_t tick)
{
    rt_atomic_store(&(rt_tick), tick);
}

#ifdef RT_USING_CPU_USAGE_TRACER
/* 按当前线程与 idle 归属，累加 user/system/idle 等统计（供 CPU 使用率跟踪） */
static void _update_process_times(rt_tick_t tick)
{
    struct rt_thread *thread = rt_thread_self();
    struct rt_cpu *pcpu = rt_cpu_self();

    if (!LWP_IS_USER_MODE(thread))
    {
        thread->user_time += tick;
        pcpu->cpu_stat.user += tick;
    }
    else
    {
        thread->system_time += tick;
        if (thread == pcpu->idle_thread)
        {
            pcpu->cpu_stat.idle += tick;
        }
        else
        {
            pcpu->cpu_stat.system += tick;
        }
    }
}

#else

#define _update_process_times(tick)
#endif /* RT_USING_CPU_USAGE_TRACER */

/**
 * @brief    This function will notify kernel there is one tick passed.
 *           Normally, this function is invoked by clock ISR.
 * @note     中文：须在时钟 ISR 中调用（断言要求中断嵌套深度 > 0）。
 *           顺序：钩子 → CPU 统计 → 累加 tick → 时间片 → 仅 CPU0 执行软件定时器检查。
 */
void rt_tick_increase(void)
{
    /* 必须在 ISR 上下文调用，避免在线程里误推进系统节拍 */
    RT_ASSERT(rt_interrupt_get_nest() > 0);

    RT_OBJECT_HOOK_CALL(rt_tick_hook, ());

    /* tracing cpu usage */
    _update_process_times(1);

    /* increase the global tick */
#ifdef RT_USING_SMP
    /* 每核维护本地 tick；CPU0 的 tick 同时映射为 rt_tick_get 使用的 rt_tick */
    rt_atomic_add(&(rt_cpu_self()->tick), 1);
#else
    rt_atomic_add(&(rt_tick), 1);
#endif /* RT_USING_SMP */

    /* check time slice */
    /* 驱动同优先级线程的时间片轮转、延时到期等调度相关逻辑 */
    rt_sched_tick_increase(1);

    /* check timer */
#ifdef RT_USING_SMP
    /* 软件定时器链表由主核统一检查，避免多核重复处理与竞态 */
    if (rt_cpu_get_id() != 0)
    {
        return;
    }
#endif
    rt_timer_check();
}

/**
 * @brief    This function will notify kernel there is n tick passed.
 *           Normally, this function is invoked by clock ISR.
 * @note     中文：一次 ISR 内补偿多个 tick（例如时钟源分频或批处理中断）时使用；
 *           SMP 下同样仅主核调用 rt_timer_check；使能 VDSO 时更新用户态可见时间。
 */
void rt_tick_increase_tick(rt_tick_t tick)
{
    RT_ASSERT(rt_interrupt_get_nest() > 0);

    RT_OBJECT_HOOK_CALL(rt_tick_hook, ());

    /* tracing cpu usage */
    _update_process_times(tick);

    /* increase the global tick */
#ifdef RT_USING_SMP
    rt_atomic_add(&(rt_cpu_self()->tick), tick);
#else
    rt_atomic_add(&(rt_tick), tick);
#endif /* RT_USING_SMP */

    /* check time slice */
    rt_sched_tick_increase(tick);

    /* check timer */
#ifdef RT_USING_SMP
    if (rt_cpu_get_id() != 0)
    {
        return;
    }
#endif
    rt_timer_check();

#ifdef RT_USING_VDSO
    /* SMART+VDSO：更新用户态通过 vdso 读取的全局时间基准 */
    rt_vdso_update_glob_time();
#endif
}

/**
 * @brief    This function will calculate the tick from millisecond.
 *
 * @param    ms is the specified millisecond.
 *              - Negative Number wait forever
 *              - Zero not wait
 *              - Max 0x7fffffff
 *
 * @return   Return the calculated tick.
 * @note     中文：负数映射为永久等待 RT_WAITING_FOREVER；非负按 RT_TICK_PER_SECOND 换算并向上取整到 tick。
 */
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
    rt_tick_t tick;

    if (ms < 0)
    {
        /* 与 IPC 超时等约定一致：负数表示一直阻塞直到被唤醒 */
        tick = (rt_tick_t)RT_WAITING_FOREVER;
    }
    else
    {
#if RT_TICK_PER_SECOND == 1000u
        tick = ms;
#else
        /* 整秒部分 + 余下毫秒按 tick 频率折算（+999 再除 1000 为向上取整到 tick） */
        tick = RT_TICK_PER_SECOND * (ms / 1000);
        tick += (RT_TICK_PER_SECOND * (ms % 1000) + 999) / 1000;
#endif /* RT_TICK_PER_SECOND == 1000u */
    }

    /* return the calculated tick */
    return tick;
}
RTM_EXPORT(rt_tick_from_millisecond);

/**
 * @brief    This function will return the passed millisecond from boot.
 *
 * @note     if the value of RT_TICK_PER_SECOND is lower than 1000 or
 *           is not an integral multiple of 1000, this function will not
 *           provide the correct 1ms-based tick.
 * @note     中文：仅当每秒 tick 数为 1000 的约数时，才能用整数乘法精确换算毫秒；
 *           否则编译告警并返回 0，需在其它文件用高精度定时器重定义本弱符号。
 *
 * @return   Return passed millisecond from boot.
 */
rt_weak rt_tick_t rt_tick_get_millisecond(void)
{
#if RT_TICK_PER_SECOND == 0 /* make cppcheck happy*/
#error "RT_TICK_PER_SECOND must be greater than zero"
#endif

#if 1000 % RT_TICK_PER_SECOND == 0u
    return rt_tick_get() * (1000u / RT_TICK_PER_SECOND);
#else
    #warning "rt-thread cannot provide a correct 1ms-based tick any longer,\
    please redefine this function in another file by using a high-precision hard-timer."
    return 0;
#endif /* 1000 % RT_TICK_PER_SECOND == 0u */
}

/**@}*/
