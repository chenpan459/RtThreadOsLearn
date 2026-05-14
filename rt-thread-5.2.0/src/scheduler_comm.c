/*
 * Copyright (c) 2006-2024 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * (scheduler_comm.c) Common API of scheduling routines.
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-18     Shell        Separate scheduling related codes from thread.c, scheduler_.*
 *
 * ---------------------------------------------------------------------------
 * 模块说明（调度公共逻辑 scheduler_comm.c）
 * ---------------------------------------------------------------------------
 * 本文件从 thread.c 拆出，提供「线程调度上下文」的读写封装：RT_SCHED_CTX 多为
 * 可变的运行态（stat、SMP 绑核等），RT_SCHED_PRIV 多为时间片与优先级等调度
 * 私有字段。除明确说明外，下列例程均假定调用方已持有调度器锁，并由
 * RT_SCHED_DEBUG_IS_LOCKED 在调试配置下断言。
 *
 * rt_sched_thread_ready 与线程定时器、挂起链表存在竞态，需先停掉挂起路径
 * 上设置的 timeout 定时器再入就绪队列。rt_sched_tick_increase 在剩余时间片
 * 耗尽时置 YIELD 并触发 unlock+resched。栈溢出检测依赖栈底/顶写入的魔数
 * '#'（或硬件栈保护），与栈增长方向宏配合。
 */

#define DBG_TAG           "kernel.sched"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#include <rtthread.h>

/* 新建线程时初始化调度侧上下文：INIT 态 + SMP 未绑核 + 私有 tick/prio */
void rt_sched_thread_init_ctx(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    /* setup thread status */
    RT_SCHED_CTX(thread).stat  = RT_THREAD_INIT;

#ifdef RT_USING_SMP
    /* not bind on any cpu */
    RT_SCHED_CTX(thread).bind_cpu = RT_CPUS_NR;
    RT_SCHED_CTX(thread).oncpu = RT_CPU_DETACHED;
#endif /* RT_USING_SMP */

    rt_sched_thread_init_priv(thread, tick, priority);
}

/* 标记本线程挂起路径上已挂起 thread_timer（实际 start 在别处完成时配合） */
rt_err_t rt_sched_thread_timer_start(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    RT_SCHED_CTX(thread).sched_flag_ttmr_set = 1;
    return RT_EOK;
}

/* 若在挂起时设过超时定时器，则 stop 并清 sched_flag_ttmr_set，避免与 resume 竞态 */
rt_err_t rt_sched_thread_timer_stop(struct rt_thread *thread)
{
    rt_err_t error;
    RT_SCHED_DEBUG_IS_LOCKED;

    if (RT_SCHED_CTX(thread).sched_flag_ttmr_set)
    {
        error = rt_timer_stop(&thread->thread_timer);

        /* mask out timer flag no matter stop success or not */
        RT_SCHED_CTX(thread).sched_flag_ttmr_set = 0;
    }
    else
    {
        error = RT_EOK;
    }
    return error;
}

/* 返回 (stat & RT_THREAD_STAT_MASK)，供查询 RUN/CLOSE 等主状态 */
rt_uint8_t rt_sched_thread_get_stat(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK;
}

rt_uint8_t rt_sched_thread_get_curr_prio(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return RT_SCHED_PRIV(thread).current_priority;
}

rt_uint8_t rt_sched_thread_get_init_prio(struct rt_thread *thread)
{
    /* init_priority 创建后不变，无需调度锁即可读 */
    return RT_SCHED_PRIV(thread).init_priority;
}

/**
 * @brief 判断线程是否处于挂起类状态（suspend 掩码全置）。
 * @note 调用方须已持调度器锁（RT_SCHED_DEBUG_IS_LOCKED）。
 */
rt_uint8_t rt_sched_thread_is_suspended(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return (RT_SCHED_CTX(thread).stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK;
}

/* 将线程标为 CLOSE，供后续清理路径使用 */
rt_err_t rt_sched_thread_close(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    RT_SCHED_CTX(thread).stat = RT_THREAD_CLOSE;
    return RT_EOK;
}

/* 时间片耗尽路径：恢复 remaining_tick 为 init_tick 并置 YIELD，请求让出 CPU */
rt_err_t rt_sched_thread_yield(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;

    RT_SCHED_PRIV(thread).remaining_tick = RT_SCHED_PRIV(thread).init_tick;
    RT_SCHED_CTX(thread).stat |= RT_THREAD_STAT_YIELD;

    return RT_EOK;
}

/* 从挂起链表唤醒入就绪队列：须先停超时定时器，避免 ISR 与 resume 双抢 */
rt_err_t rt_sched_thread_ready(struct rt_thread *thread)
{
    rt_err_t error;

    RT_SCHED_DEBUG_IS_LOCKED;

    if (!rt_sched_thread_is_suspended(thread))
    {
        /* failed to proceed, and that's possibly due to a racing condition */
        error = -RT_EINVAL;
    }
    else
    {
        if (RT_SCHED_CTX(thread).sched_flag_ttmr_set)
        {
            /**
             * Quiet timeout timer first if set. and don't continue if we
             * failed, because it probably means that a timeout ISR racing to
             * resume thread before us.
             */
            error = rt_sched_thread_timer_stop(thread);
        }
        else
        {
            error = RT_EOK;
        }

        if (!error)
        {
            /* remove from suspend list */
            rt_list_remove(&RT_THREAD_LIST_NODE(thread));

        #ifdef RT_USING_SMART
            thread->wakeup_handle.func = RT_NULL;
        #endif

            /* insert to schedule ready list and remove from susp list */
            rt_sched_insert_thread(thread);
        }
    }

    return error;
}

/* 当前线程时间片扣减；耗尽则 yield 并在持锁情况下请求一次抢占调度 */
rt_err_t rt_sched_tick_increase(rt_tick_t tick)
{
    struct rt_thread *thread;
    rt_sched_lock_level_t slvl;

    thread = rt_thread_self();

    rt_sched_lock(&slvl);

    if(RT_SCHED_PRIV(thread).remaining_tick > tick)
    {
        RT_SCHED_PRIV(thread).remaining_tick -= tick;
    }
    else
    {
        RT_SCHED_PRIV(thread).remaining_tick = 0;
    }

    if (RT_SCHED_PRIV(thread).remaining_tick)
    {
        rt_sched_unlock(slvl);
    }
    else
    {
        rt_sched_thread_yield(thread);

        /* request a rescheduling even though we are probably in an ISR */
        rt_sched_unlock_n_resched(slvl);
    }

    return RT_EOK;
}

/**
 * @brief Update priority of the target thread
 * @note 调用方须持调度器锁。若线程已在就绪态则摘队、改 current_priority、
 *       重算 number_mask/high_mask，将 stat 置 INIT 后经 rt_sched_insert_thread 再入队；
 *       非就绪则仅更新优先级域，避免破坏挂起/延时等状态。
 */
rt_err_t rt_sched_thread_change_priority(struct rt_thread *thread, rt_uint8_t priority)
{
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    RT_SCHED_DEBUG_IS_LOCKED;

    /* for ready thread, change queue; otherwise simply update the priority */
    if ((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_READY)
    {
        /* remove thread from schedule queue first */
        rt_sched_remove_thread(thread);

        /* change thread priority */
        RT_SCHED_PRIV(thread).current_priority = priority;

        /* recalculate priority attribute */
#if RT_THREAD_PRIORITY_MAX > 32
        RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;               /* 5bit */
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).number;
        RT_SCHED_PRIV(thread).high_mask = 1 << (RT_SCHED_PRIV(thread).current_priority & 0x07);   /* 3bit */
#else
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
        RT_SCHED_CTX(thread).stat = RT_THREAD_INIT;

        /* insert thread to schedule queue again */
        rt_sched_insert_thread(thread);
    }
    else
    {
        RT_SCHED_PRIV(thread).current_priority = priority;

        /* recalculate priority attribute */
#if RT_THREAD_PRIORITY_MAX > 32
        RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;               /* 5bit */
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).number;
        RT_SCHED_PRIV(thread).high_mask = 1 << (RT_SCHED_PRIV(thread).current_priority & 0x07);   /* 3bit */
#else
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    }

    return RT_EOK;
}

#ifdef RT_USING_OVERFLOW_CHECK
/* 在上下文切换等路径调用：检查 '#' 哨兵与 sp 是否在栈区间内；Smart 用户栈特例跳过 */
void rt_scheduler_stack_check(struct rt_thread *thread)
{
    RT_ASSERT(thread != RT_NULL);

#ifdef RT_USING_SMART
#ifndef ARCH_MM_MMU
    struct rt_lwp *lwp = thread ? (struct rt_lwp *)thread->lwp : 0;

    /* 无 MMU 时 SP 可能落在进程 data 段表示在用户态上下文，不做内核栈魔数校验 */
    if (lwp && ((rt_uint32_t)thread->sp > (rt_uint32_t)lwp->data_entry &&
    (rt_uint32_t)thread->sp <= (rt_uint32_t)lwp->data_entry + (rt_uint32_t)lwp->data_size))
    {
        return;
    }
#endif /* not defined ARCH_MM_MMU */
#endif /* RT_USING_SMART */

#ifndef RT_USING_HW_STACK_GUARD
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
    if (*((rt_uint8_t *)((rt_uintptr_t)thread->stack_addr + thread->stack_size - 1)) != '#' ||
#else
    if (*((rt_uint8_t *)thread->stack_addr) != '#' ||
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */
        (rt_uintptr_t)thread->sp <= (rt_uintptr_t)thread->stack_addr ||
        (rt_uintptr_t)thread->sp >
        (rt_uintptr_t)thread->stack_addr + (rt_uintptr_t)thread->stack_size)
    {
        rt_base_t dummy = 1;

        LOG_E("thread:%s stack overflow\n", thread->parent.name);

        while (dummy);
    }
#endif /* RT_USING_HW_STACK_GUARD */
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
#ifndef RT_USING_HW_STACK_GUARD
    else if ((rt_uintptr_t)thread->sp > ((rt_uintptr_t)thread->stack_addr + thread->stack_size))
#else
    if ((rt_uintptr_t)thread->sp > ((rt_uintptr_t)thread->stack_addr + thread->stack_size))
#endif
    {
        LOG_W("warning: %s stack is close to the top of stack address.\n",
                   thread->parent.name);
    }
#else
#ifndef RT_USING_HW_STACK_GUARD
    else if ((rt_uintptr_t)thread->sp <= ((rt_uintptr_t)thread->stack_addr + 32))
#else
    if ((rt_uintptr_t)thread->sp <= ((rt_uintptr_t)thread->stack_addr + 32))
#endif
    {
        LOG_W("warning: %s stack is close to end of stack address.\n",
                   thread->parent.name);
    }
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */
}

#endif /* RT_USING_OVERFLOW_CHECK */
