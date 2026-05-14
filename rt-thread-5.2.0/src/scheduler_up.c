/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-17     Bernard      the first version
 * 2006-04-28     Bernard      fix the scheduler algorthm
 * 2006-04-30     Bernard      add SCHEDULER_DEBUG
 * 2006-05-27     Bernard      fix the scheduler algorthm for same priority
 *                             thread schedule
 * 2006-06-04     Bernard      rewrite the scheduler algorithm
 * 2006-08-03     Bernard      add hook support
 * 2006-09-05     Bernard      add 32 priority level support
 * 2006-09-24     Bernard      add rt_system_scheduler_start function
 * 2009-09-16     Bernard      fix _rt_scheduler_stack_check
 * 2010-04-11     yi.qiu       add module feature
 * 2010-07-13     Bernard      fix the maximal number of rt_scheduler_lock_nest
 *                             issue found by kuronca
 * 2010-12-13     Bernard      add defunct list initialization even if not use heap.
 * 2011-05-10     Bernard      clean scheduler debug log.
 * 2013-12-21     Grissiom     add rt_critical_level
 * 2018-11-22     Jesven       remove the current task from ready queue
 *                             add per cpu ready queue
 *                             add _scheduler_get_highest_priority_thread to find highest priority task
 *                             rt_schedule_insert_thread won't insert current task to ready queue
 *                             in smp version, rt_hw_context_switch_interrupt maybe switch to
 *                             new task directly
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to scheduler.c
 * 2023-03-27     rose_man     Split into scheduler upc and scheduler_mp.c
 * 2023-10-17     ChuShicheng  Modify the timing of clearing RT_THREAD_STAT_YIELD flag bits
 *
 * ---------------------------------------------------------------------------
 * 单核调度器（scheduler_up.c，非 RT_USING_SMP）
 * ---------------------------------------------------------------------------
 * 与 scheduler_mp.c 对应：仅一套就绪优先级表 rt_thread_priority_table[] 与
 * rt_thread_ready_priority_group 位图，无绑核/IPI/每核就绪队列。插入与摘除
 * 就绪线程用「关中断」保护临界区，而非 MP 的全局调度自旋锁。
 *
 * rt_scheduler_lock_nest：rt_enter_critical/rt_exit_critical 维护；仅当 nest==0
 * 时 rt_schedule 才真正挑选线程（与 MP 里每线程 critical_lock_nest 不同）。
 * rt_sched_lock/rt_sched_unlock 在 UP 上仅保存/恢复中断掩码，不含 nest 变化；
 * rt_sched_unlock_n_resched 在恢复中断前调用 rt_schedule()。
 *
 * rt_current_priority：记录当前被选中运行线程的优先级数值，供内核其它路径查询。
 */

#define __RT_IPC_SOURCE__
#include <rtthread.h>
#include <rthw.h>

#define DBG_TAG           "kernel.scheduler"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/* 单核就绪队列：每优先级一条双向链表头 + 位图表示哪些优先级非空 */
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
rt_uint32_t rt_thread_ready_priority_group;
#if RT_THREAD_PRIORITY_MAX > 32
/* Maximum priority level, 256 */
rt_uint8_t rt_thread_ready_table[32];
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

extern volatile rt_atomic_t rt_interrupt_nest;
/* rt_enter/exit_critical 嵌套计数；>0 时 rt_schedule 直接跳过 */
static rt_int16_t rt_scheduler_lock_nest;
/* 最近一次调度选中的优先级（数值越小优先级越高，依 RT_THREAD_PRIORITY_MAX 定义） */
rt_uint8_t rt_current_priority;

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_scheduler_hook)(struct rt_thread *from, struct rt_thread *to);
static void (*rt_scheduler_switch_hook)(struct rt_thread *tid);

/**
 * @addtogroup group_Hook
 */

/**@{*/

/**
 * @brief This function will set a hook function, which will be invoked when thread
 *        switch happens.
 *
 * @param hook is the hook function.
 * @note 选定 to 线程、即将切换前调用。
 */
void rt_scheduler_sethook(void (*hook)(struct rt_thread *from, struct rt_thread *to))
{
    rt_scheduler_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when context
 *        switch happens.
 *
 * @param hook is the hook function.
 * @note 切离 from 线程、进入汇编切换前调用。
 */
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid))
{
    rt_scheduler_switch_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/* 由就绪位图 __rt_ffs 得最高优先级，再取该优先级链表首结点为下一个候选线程 */
static struct rt_thread* _scheduler_get_highest_priority_thread(rt_ubase_t *highest_prio)
{
    struct rt_thread *highest_priority_thread;
    rt_ubase_t highest_ready_priority;

#if RT_THREAD_PRIORITY_MAX > 32
    rt_ubase_t number;

    number = __rt_ffs(rt_thread_ready_priority_group) - 1;
    highest_ready_priority = (number << 3) + __rt_ffs(rt_thread_ready_table[number]) - 1;
#else
    highest_ready_priority = __rt_ffs(rt_thread_ready_priority_group) - 1;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* get highest ready priority thread */
    highest_priority_thread = RT_THREAD_LIST_NODE_ENTRY(rt_thread_priority_table[highest_ready_priority].next);

    *highest_prio = highest_ready_priority;

    return highest_priority_thread;
}

/* 仅关/开中断保存点，不改变 rt_scheduler_lock_nest；与 MP 版语义不同 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl)
{
    rt_base_t level;
    if (!plvl)
        return -RT_EINVAL;

    level = rt_hw_interrupt_disable();
    *plvl = level;

    return RT_EOK;
}

rt_err_t rt_sched_unlock(rt_sched_lock_level_t level)
{
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

/* 在恢复中断前尝试调度一次，便于持 plvl 临界区内完成状态更新后再切换 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level)
{
    if (rt_thread_self())
    {
        /* if scheduler is available */
        rt_schedule();
    }
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

/**
 * @brief This function will initialize the system scheduler.
 * @note 清空 nest，初始化各优先级链表头与就绪位图。
 */
void rt_system_scheduler_init(void)
{
    rt_base_t offset;
    rt_scheduler_lock_nest = 0;

    LOG_D("start scheduler: max priority 0x%02x",
          RT_THREAD_PRIORITY_MAX);

    for (offset = 0; offset < RT_THREAD_PRIORITY_MAX; offset ++)
    {
        rt_list_init(&rt_thread_priority_table[offset]);
    }

    /* initialize ready priority group */
    rt_thread_ready_priority_group = 0;

#if RT_THREAD_PRIORITY_MAX > 32
    /* initialize ready table */
    rt_memset(rt_thread_ready_table, 0, sizeof(rt_thread_ready_table));
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
}

/**
 * @brief This function will startup the scheduler. It will select one thread
 *        with the highest priority level, then switch to it.
 * @note 设置 current_thread、摘就绪链、置 RUNNING 后 rt_hw_context_switch_to。
 */
void rt_system_scheduler_start(void)
{
    struct rt_thread *to_thread;
    rt_ubase_t highest_ready_priority;

    to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

    rt_cpu_self()->current_thread = to_thread;

    rt_sched_remove_thread(to_thread);
    RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING;

    /* switch to new thread */

    rt_hw_context_switch_to((rt_uintptr_t)&to_thread->sp);

    /* never come back */
}

/**
 * @addtogroup group_Thread
 * @cond
 */

/**@{*/

/**
 * @brief This function will perform scheduling once. It will select one thread
 *        with the highest priority, and switch to it immediately.
 * @note rt_scheduler_lock_nest!=0 时整段调度跳过。比较当前 RUNNING 与最高
 *       就绪优先级及 YIELD 位决定是否继续占用 CPU；切换前按需把 from 插回就绪队列。
 *       rt_interrupt_nest!=0 时走 rt_hw_context_switch_interrupt。YIELD 标志在
 *       确定切换目标后清除（ChuShicheng 2023 调整时机）。
 */
void rt_schedule(void)
{
    rt_base_t level;
    struct rt_thread *to_thread;
    struct rt_thread *from_thread;
    /* 缓存 self 指针，避免多次 rt_thread_self() */
    struct rt_thread *curr_thread = rt_thread_self();

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    /* check the scheduler is enabled or not */
    if (rt_scheduler_lock_nest == 0)
    {
        rt_ubase_t highest_ready_priority;

        if (rt_thread_ready_priority_group != 0)
        {
            /* need_insert_from_thread: need to insert from_thread to ready queue */
            int need_insert_from_thread = 0;

            to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

            if ((RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_RUNNING)
            {
                if (RT_SCHED_PRIV(curr_thread).current_priority < highest_ready_priority)
                {
                    to_thread = curr_thread;
                }
                else if (RT_SCHED_PRIV(curr_thread).current_priority == highest_ready_priority
                         && (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_YIELD_MASK) == 0)
                {
                    to_thread = curr_thread;
                }
                else
                {
                    need_insert_from_thread = 1;
                }
            }

            if (to_thread != curr_thread)
            {
                /* if the destination thread is not the same as current thread */
                rt_current_priority = (rt_uint8_t)highest_ready_priority;
                from_thread                   = curr_thread;
                rt_cpu_self()->current_thread = to_thread;

                RT_OBJECT_HOOK_CALL(rt_scheduler_hook, (from_thread, to_thread));

                if (need_insert_from_thread)
                {
                    rt_sched_insert_thread(from_thread);
                }

                if ((RT_SCHED_CTX(from_thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
                {
                    RT_SCHED_CTX(from_thread).stat &= ~RT_THREAD_STAT_YIELD_MASK;
                }

                rt_sched_remove_thread(to_thread);
                RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(to_thread).stat & ~RT_THREAD_STAT_MASK);

                /* switch to new thread */
                LOG_D("[%d]switch to priority#%d "
                         "thread:%.*s(sp:0x%08x), "
                         "from thread:%.*s(sp: 0x%08x)",
                         rt_interrupt_nest, highest_ready_priority,
                         RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                         RT_NAME_MAX, from_thread->parent.name, from_thread->sp);

                RT_SCHEDULER_STACK_CHECK(to_thread);

                if (rt_interrupt_nest == 0)
                {
                    extern void rt_thread_handle_sig(rt_bool_t clean_state);

                    RT_OBJECT_HOOK_CALL(rt_scheduler_switch_hook, (from_thread));

                    rt_hw_context_switch((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp);

                    /* enable interrupt */
                    rt_hw_interrupt_enable(level);

#ifdef RT_USING_SIGNALS
                    /* check stat of thread for signal */
                    level = rt_hw_interrupt_disable();
                    if (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
                    {
                        extern void rt_thread_handle_sig(rt_bool_t clean_state);

                        RT_SCHED_CTX(curr_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

                        rt_hw_interrupt_enable(level);

                        /* check signal status */
                        rt_thread_handle_sig(RT_TRUE);
                    }
                    else
                    {
                        rt_hw_interrupt_enable(level);
                    }
#endif /* RT_USING_SIGNALS */
                    goto __exit;
                }
                else
                {
                    LOG_D("switch in interrupt");

                    rt_hw_context_switch_interrupt((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp, from_thread, to_thread);
                }
            }
            else
            {
                rt_sched_remove_thread(curr_thread);
                RT_SCHED_CTX(curr_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(curr_thread).stat & ~RT_THREAD_STAT_MASK);
            }
        }
    }

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

__exit:
    return;
}

/* 与 MP 类似：填优先级位图掩码并置 SUSPEND，等待 startup/resume 进入调度 */
void rt_sched_thread_startup(struct rt_thread *thread)
{
#if RT_THREAD_PRIORITY_MAX > 32
    RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;            /* 5bit */
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).number;
    RT_SCHED_PRIV(thread).high_mask = 1L << (RT_SCHED_PRIV(thread).current_priority & 0x07);  /* 3bit */
#else
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* change thread stat, so we can resume it */
    RT_SCHED_CTX(thread).stat = RT_THREAD_SUSPEND;
}

/* 线程对象创建阶段初始化链表节点、优先级与时间片；不入就绪队列 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    rt_list_init(&RT_THREAD_LIST_NODE(thread));

    /* priority init */
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    RT_SCHED_PRIV(thread).init_priority    = priority;
    RT_SCHED_PRIV(thread).current_priority = priority;

    /* don't add to scheduler queue as init thread */
    RT_SCHED_PRIV(thread).number_mask = 0;
#if RT_THREAD_PRIORITY_MAX > 32
    RT_SCHED_PRIV(thread).number = 0;
    RT_SCHED_PRIV(thread).high_mask = 0;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* tick init */
    RT_SCHED_PRIV(thread).init_tick = tick;
    RT_SCHED_PRIV(thread).remaining_tick = tick;
}

/**
 * @brief This function will insert a thread to the system ready queue. The state of
 *        thread will be set as READY and the thread will be removed from suspend queue.
 *
 * @param thread is the thread to be inserted.
 *
 * @note  Please do not invoke this function in user application.
 * @note  关中断内操作。若 thread 已是 rt_current_thread 则只修正为 RUNNING
 *        不入队（避免当前线程重复进就绪表）。
 */
void rt_sched_insert_thread(struct rt_thread *thread)
{
    rt_base_t level;

    RT_ASSERT(thread != RT_NULL);

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    /* it's current thread, it should be RUNNING thread */
    if (thread == rt_current_thread)
    {
        RT_SCHED_CTX(thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
        goto __exit;
    }

    /* READY thread, insert to ready queue */
    RT_SCHED_CTX(thread).stat = RT_THREAD_READY | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
    /* there is no time slices left(YIELD), inserting thread before ready list*/
    if((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
    {
        rt_list_insert_before(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }
    /* there are some time slices left, inserting thread after ready list to schedule it firstly at next time*/
    else
    {
        rt_list_insert_after(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }

    LOG_D("insert thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name, RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* set priority mask */
#if RT_THREAD_PRIORITY_MAX > 32
    rt_thread_ready_table[RT_SCHED_PRIV(thread).number] |= RT_SCHED_PRIV(thread).high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    rt_thread_ready_priority_group |= RT_SCHED_PRIV(thread).number_mask;

__exit:
    /* enable interrupt */
    rt_hw_interrupt_enable(level);
}

/**
 * @brief This function will remove a thread from system ready queue.
 *
 * @param thread is the thread to be removed.
 *
 * @note  Please do not invoke this function in user application.
 * @note  关中断内仅从就绪链摘除并维护位图；不修改 stat（与 MP 版不同）。
 */
void rt_sched_remove_thread(struct rt_thread *thread)
{
    rt_base_t level;

    RT_ASSERT(thread != RT_NULL);

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    LOG_D("remove thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name,
          RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* remove thread from ready list */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    if (rt_list_isempty(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority])))
    {
#if RT_THREAD_PRIORITY_MAX > 32
        rt_thread_ready_table[RT_SCHED_PRIV(thread).number] &= ~RT_SCHED_PRIV(thread).high_mask;
        if (rt_thread_ready_table[RT_SCHED_PRIV(thread).number] == 0)
        {
            rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
        }
#else
        rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    }

    /* enable interrupt */
    rt_hw_interrupt_enable(level);
}

#ifdef RT_DEBUGING_CRITICAL

static volatile int _critical_error_occurred = 0;

/* 调试：校验传入 nest 与全局 rt_scheduler_lock_nest 一致 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    rt_base_t level;
    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    if (!_critical_error_occurred)
    {
        if (critical_level != rt_scheduler_lock_nest)
        {
            int dummy = 1;
            _critical_error_occurred = 1;

            rt_kprintf("%s: un-compatible critical level\n" \
                       "\tCurrent %d\n\tCaller %d\n",
                       __func__, rt_scheduler_lock_nest,
                       critical_level);
            rt_backtrace();

            while (dummy) ;
        }
    }
    rt_hw_interrupt_enable(level);

    rt_exit_critical();
}

#else /* !RT_DEBUGING_CRITICAL */

void rt_exit_critical_safe(rt_base_t critical_level)
{
    RT_UNUSED(critical_level);
    rt_exit_critical();
}

#endif/* RT_DEBUGING_CRITICAL */
RTM_EXPORT(rt_exit_critical_safe);

/**
 * @brief This function will lock the thread scheduler.
 * @note 递增全局 rt_scheduler_lock_nest，禁止隐式调度；与 MP 每线程 nest 不同。
 */
rt_base_t rt_enter_critical(void)
{
    rt_base_t level;
    rt_base_t critical_level;

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    /*
     * the maximal number of nest is RT_UINT16_MAX, which is big
     * enough and does not check here
     */
    rt_scheduler_lock_nest ++;
    critical_level = rt_scheduler_lock_nest;

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    return critical_level;
}
RTM_EXPORT(rt_enter_critical);

/**
 * @brief This function will unlock the thread scheduler.
 * @note nest 归零且已有 current 线程时再 rt_schedule()，避免启动前误调度。
 */
void rt_exit_critical(void)
{
    rt_base_t level;

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    rt_scheduler_lock_nest --;
    if (rt_scheduler_lock_nest <= 0)
    {
        rt_scheduler_lock_nest = 0;
        /* enable interrupt */
        rt_hw_interrupt_enable(level);

        if (rt_current_thread)
        {
            /* if scheduler is started, do a schedule */
            rt_schedule();
        }
    }
    else
    {
        /* enable interrupt */
        rt_hw_interrupt_enable(level);
    }
}
RTM_EXPORT(rt_exit_critical);

/**
 * @brief Get the scheduler lock level.
 *
 * @return the level of the scheduler lock. 0 means unlocked.
 * @note 返回全局 rt_scheduler_lock_nest，即 rt_enter_critical 嵌套深度。
 */
rt_uint16_t rt_critical_level(void)
{
    return rt_scheduler_lock_nest;
}
RTM_EXPORT(rt_critical_level);

/* 单核无绑核语义，与 SMP API 保持符号兼容 */
rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu)
{
    RT_UNUSED(thread);
    RT_UNUSED(cpu);
    return -RT_EINVAL;
}

/**@}*/
/**@endcond*/
