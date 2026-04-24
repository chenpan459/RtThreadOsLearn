/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017/10/5      Bernard      the first version
 * 2018/09/17     Jesven       fix: in _signal_deliver RT_THREAD_STAT_MASK to RT_THREAD_STAT_SIGNAL_MASK
 * 2018/11/22     Jesven       in smp version rt_hw_context_switch_to add a param
 *
 * ---------------------------------------------------------------------------
 * 模块说明（RT_USING_SIGNALS）
 * ---------------------------------------------------------------------------
 * 为线程提供类 POSIX 的软信号：pending 位图 sig_pending、屏蔽字 sig_mask、
 * 每号处理函数表 sig_vectors，以及挂链的 siginfo 节点（含 si_signo 等）。
 * 全局内存池 _siginfo_pool 分配 struct siginfo_node，避免在投递路径频繁
 * rt_malloc；跨线程与调度器交互由 _thread_signal_lock 保护。
 *
 * 投递 _signal_deliver：挂起线程则 resume/wakeup 并置 SIGNAL/SIGNAL_PENDING；
 * 就绪且为当前线程则在非中断上下文直接 rt_thread_handle_sig；就绪且为其它
 * 线程则 UP 上伪造小栈帧切入 _signal_entry，SMP 上由 rt_signal_check 在合适
 * 时机切栈。sig_mask 在 MUSL 与默认下编号映射不同（见宏 sig_mask）。
 */

#include <stdint.h>
#include <string.h>

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_SIGNALS

/* siginfo 节点池容量，64 位默认略大以容纳指针型 si_value */

#ifndef RT_SIG_INFO_MAX
    #ifdef ARCH_CPU_64BIT
        #define RT_SIG_INFO_MAX 64
    #else
        #define RT_SIG_INFO_MAX 32
    #endif /* ARCH_CPU_64BIT */
#endif /* RT_SIG_INFO_MAX */

#define DBG_TAG     "SIGN"
#define DBG_LVL     DBG_WARNING
#include <rtdbg.h>

/* MUSL 约定 signo 从 1 起；否则与位号一致，与 rt_thread_kill 等传入值对齐 */
#ifdef RT_USING_MUSLLIBC
    #define sig_mask(sig_no)    (1u << (sig_no - 1))
#else
    #define sig_mask(sig_no)    (1u << sig_no)
#endif
#define sig_valid(sig_no)   (sig_no >= 0 && sig_no < RT_SIG_MAX)

static struct rt_spinlock _thread_signal_lock = RT_SPINLOCK_INIT;

/* 每线程 si_list 上的元素：内嵌 siginfo_t + 单链表结点 */
struct siginfo_node
{
    siginfo_t si;
    struct rt_slist_node list;
};

static struct rt_mempool *_siginfo_pool;
static void _signal_deliver(rt_thread_t tid);
void rt_thread_handle_sig(rt_bool_t clean_state);

/* SIG_DFL 安装时指向该函数，仅打日志不终止线程 */
static void _signal_default_handler(int signo)
{
    RT_UNUSED(signo);
    LOG_I("handled signo[%d] with default action.", signo);
    return ;
}

/* 伪造栈顶进入的跳板：先处理信号再恢复原 sp（UP）或切回线程（SMP 经 context_switch_to） */
static void _signal_entry(void *parameter)
{
    RT_UNUSED(parameter);

    rt_thread_t tid = rt_thread_self();

    /* 在用户构造的「信号栈」上跑 handler */
    rt_thread_handle_sig(RT_FALSE);

#ifdef RT_USING_SMP
#else
    /* return to thread */
    tid->sp = tid->sig_ret;
    tid->sig_ret = RT_NULL;
#endif /* RT_USING_SMP */

    LOG_D("switch back to: 0x%08x\n", tid->sp);
    RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL;

#ifdef RT_USING_SMP
    rt_hw_context_switch_to((rt_uintptr_t)&parameter, tid);
#else
    rt_hw_context_switch_to((rt_uintptr_t)&(tid->sp));
#endif /* RT_USING_SMP */
}

/*
 * 投递策略摘要：
 * 1) 挂起：resume/wakeup，置 SIGNAL|SIGNAL_PENDING，再 schedule。
 * 2) 就绪且为 self：置 SIGNAL；非中断里直接 handle_sig。
 * 3) 就绪且为其它线程且尚未在信号处理中：置 PENDING；UP 保存 sig_ret 并
 *    把 sp 切到 _signal_entry 小栈；SMP 发 IPI 由 rt_signal_check 切上下文。
 */
static void _signal_deliver(rt_thread_t tid)
{
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* thread is not interested in pended signals */
    if (!(tid->sig_pending & tid->sig_mask))
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        return;
    }

    if ((RT_SCHED_CTX(tid).stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK)
    {
        /* resume thread to handle signal */
#ifdef RT_USING_SMART
        rt_thread_wakeup(tid);
#else
        rt_thread_resume(tid);
#endif
        /* add signal state */
        RT_SCHED_CTX(tid).stat |= (RT_THREAD_STAT_SIGNAL | RT_THREAD_STAT_SIGNAL_PENDING);

        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        /* re-schedule */
        rt_schedule();
    }
    else
    {
        if (tid == rt_thread_self())
        {
            /* add signal state */
            RT_SCHED_CTX(tid).stat |= RT_THREAD_STAT_SIGNAL;

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

            /* do signal action in self thread context */
            if (rt_interrupt_get_nest() == 0)
            {
                rt_thread_handle_sig(RT_TRUE);
            }
        }
        else if (!((RT_SCHED_CTX(tid).stat & RT_THREAD_STAT_SIGNAL_MASK) & RT_THREAD_STAT_SIGNAL))
        {
            /* add signal state */
            RT_SCHED_CTX(tid).stat |= (RT_THREAD_STAT_SIGNAL | RT_THREAD_STAT_SIGNAL_PENDING);

#ifdef RT_USING_SMP
            {
                int cpu_id;

                cpu_id = RT_SCHED_CTX(tid).oncpu;
                if ((cpu_id != RT_CPU_DETACHED) && (cpu_id != rt_cpu_get_id()))
                {
                    rt_uint32_t cpu_mask;

                    cpu_mask = RT_CPU_MASK ^ (1 << cpu_id);
                    rt_hw_ipi_send(RT_SCHEDULE_IPI, cpu_mask);
                }
            }
#else
            /* point to the signal handle entry */
            RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;
            tid->sig_ret = tid->sp;
            tid->sp = rt_hw_stack_init((void *)_signal_entry, RT_NULL,
                                       (void *)((char *)tid->sig_ret - 32), RT_NULL);
#endif /* RT_USING_SMP */

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
            LOG_D("signal stack pointer @ 0x%08x", tid->sp);

            /* re-schedule */
            rt_schedule();
        }
        else
        {
            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        }
    }
}

#ifdef RT_USING_SMP
/* 中断/异常出口：在已持 cpus 锁且非嵌套中断时，为 PENDING 信号构造 _signal_entry 新栈顶 */
void *rt_signal_check(void* context)
{
    rt_sched_lock_level_t level;
    int cpu_id;
    struct rt_cpu* pcpu;
    struct rt_thread *current_thread;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    cpu_id = rt_cpu_get_id();
    pcpu   = rt_cpu_index(cpu_id);
    current_thread = pcpu->current_thread;

    if (pcpu->irq_nest)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        return context;
    }

    if (current_thread->cpus_lock_nest == 1)
    {
        if (RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
        {
            void *sig_context;

            RT_SCHED_CTX(current_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
            sig_context = rt_hw_stack_init((void *)_signal_entry, context,
                    (void*)((char*)context - 32), RT_NULL);
            return sig_context;
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    return context;
}
#endif /* RT_USING_SMP */

/**
 * @brief    This function will install a processing function to a specific
 *           signal and return the old processing function of this signal.
 *
 * @note     This function needs to be used in conjunction with the
 *           rt_signal_unmask() function to make the signal effective.
 *
 * @see      rt_signal_unmask()
 *
 * @param    signo is a specific signal value (range: 0 ~ RT_SIG_MAX).
 *
 * @param    handler is sets the processing of signal value.
 *
 * @return   Return the old processing function of this signal. ONLY When the
 *           return value is SIG_ERR, the operation is failed.
 * @note     中文：首次安装会 rt_thread_alloc_sig 分配向量表；SIG_IGN/SIG_DFL
 *           分别清空或默认处理。
 */
rt_sighandler_t rt_signal_install(int signo, rt_sighandler_t handler)
{
    rt_base_t level;
    rt_sighandler_t old = RT_NULL;
    rt_thread_t tid = rt_thread_self();

    if (!sig_valid(signo)) return SIG_ERR;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_vectors == RT_NULL)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        rt_thread_alloc_sig(tid);

        level = rt_spin_lock_irqsave(&_thread_signal_lock);
    }

    if (tid->sig_vectors)
    {
        old = tid->sig_vectors[signo];

        if (handler == SIG_IGN) tid->sig_vectors[signo] = RT_NULL;
        else if (handler == SIG_DFL) tid->sig_vectors[signo] = _signal_default_handler;
        else tid->sig_vectors[signo] = handler;
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    return old;
}

/**
 * @brief    This function will block the specified signal.
 *
 * @note     This function will block the specified signal, even if the
 *           rt_thread_kill() function is called to send this signal to
 *           the current thread, it will no longer take effect.
 *
 * @see      rt_thread_kill()
 *
 * @param    signo is a specific signal value (range: 0 ~ RT_SIG_MAX).
 * @note     中文：清除 sig_mask 中对应位，使 pending&mask 不再匹配（屏蔽该号）。
 */
void rt_signal_mask(int signo)
{
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    tid->sig_mask &= ~sig_mask(signo);

    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
}

/**
 * @brief    This function will unblock the specified signal.
 *
 * @note     This function will unblock the specified signal. After calling
 *           the rt_thread_kill() function to send this signal to the current
 *           thread, it will take effect.
 *
 * @see      rt_thread_kill()
 *
 * @param    signo is a specific signal value (range: 0 ~ RT_SIG_MAX).
 * @note     中文：置位 sig_mask 允许该号；若已有 pending 则立即 _signal_deliver。
 */
void rt_signal_unmask(int signo)
{
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    tid->sig_mask |= sig_mask(signo);

    /* let thread handle pended signals */
    if (tid->sig_mask & tid->sig_pending)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        _signal_deliver(tid);
    }
    else
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    }
}

/**
 * @brief    This function will wait for the arrival of the set signal. If it does not wait for this signal, the thread will be
 *           suspended until it waits for this signal or the waiting time exceeds the specified timeout: timeout.
 *
 * @param    set is the set of signal values to be waited for. Use the function
 *           sigaddset() to add the signal.
 *
 * @param    si is a pointer to the received signal info. If you don't care about this value, you can use RT_NULL to set.
 *
 * @param    timeout is a timeout period (unit: an OS tick).
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the signal wait failed.
 * @note     中文：在 *set 与 pending 无交集时可阻塞；唤醒后从 si_list 摘首个
 *           匹配 siginfo 拷贝到 *si 并清 pending 位。timeout==0 且未就绪则 -RT_ETIMEOUT。
 */
int rt_signal_wait(const rt_sigset_t *set, rt_siginfo_t *si, rt_int32_t timeout)
{
    int ret = RT_EOK;
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();
    struct siginfo_node *si_node = RT_NULL, *si_prev = RT_NULL;

    /* current context checking */
    RT_DEBUG_IN_THREAD_CONTEXT;

    /* parameters check */
    if (set == NULL || *set == 0 || si == NULL )
    {
        ret = -RT_EINVAL;
        goto __done_return;
    }

    /* clear siginfo to avoid unknown value */
    memset(si, 0x0, sizeof(rt_siginfo_t));

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* already pending */
    if (tid->sig_pending & *set) goto __done;

    if (timeout == 0)
    {
        ret = -RT_ETIMEOUT;
        goto __done_int;
    }

    /* suspend self thread */
    rt_thread_suspend_with_flag(tid, RT_UNINTERRUPTIBLE);
    /* set thread stat as waiting for signal */
    RT_SCHED_CTX(tid).stat |= RT_THREAD_STAT_SIGNAL_WAIT;

    /* start timeout timer */
    if (timeout != RT_WAITING_FOREVER)
    {
        /* reset the timeout of thread timer and start it */
        rt_timer_control(&(tid->thread_timer),
                         RT_TIMER_CTRL_SET_TIME,
                         &timeout);
        rt_timer_start(&(tid->thread_timer));
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    /* do thread scheduling */
    rt_schedule();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* remove signal waiting flag */
    RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL_WAIT;

    /* check errno of thread */
    if (tid->error == -RT_ETIMEOUT)
    {
        tid->error = RT_EOK;
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        /* timer timeout */
        ret = -RT_ETIMEOUT;
        goto __done_return;
    }

__done:
    /* to get the first matched pending signals */
    si_node = (struct siginfo_node *)tid->si_list;
    while (si_node)
    {
        int signo;

        signo = si_node->si.si_signo;
        if (sig_mask(signo) & *set)
        {
            *si  = si_node->si;

            LOG_D("sigwait: %d sig raised!", signo);
            if (si_prev) si_prev->list.next = si_node->list.next;
            else
            {
                struct siginfo_node *node_next;

                if (si_node->list.next)
                {
                    node_next = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);
                    tid->si_list = node_next;
                }
                else
                {
                    tid->si_list = RT_NULL;
                }
            }

            /* clear pending */
            tid->sig_pending &= ~sig_mask(signo);
            rt_mp_free(si_node);
            break;
        }

        si_prev = si_node;
        if (si_node->list.next)
        {
            si_node = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);
        }
        else
        {
            si_node = RT_NULL;
        }
     }

__done_int:
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

__done_return:
    return ret;
}

/* 遍历 si_list：在锁外调 handler，避免死锁；clean_state==RT_TRUE 时清除 SIGNAL 位 */
void rt_thread_handle_sig(rt_bool_t clean_state)
{
    rt_base_t level;

    rt_thread_t tid = rt_thread_self();
    struct siginfo_node *si_node;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_pending & tid->sig_mask)
    {
        /* if thread is not waiting for signal */
        if (!(RT_SCHED_CTX(tid).stat & RT_THREAD_STAT_SIGNAL_WAIT))
        {
            while (tid->sig_pending & tid->sig_mask)
            {
                int signo, error;
                rt_sighandler_t handler;

                si_node = (struct siginfo_node *)tid->si_list;
                if (!si_node) break;

                /* remove this sig info node from list */
                if (si_node->list.next == RT_NULL)
                    tid->si_list = RT_NULL;
                else
                    tid->si_list = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);

                signo   = si_node->si.si_signo;
                handler = tid->sig_vectors[signo];
                tid->sig_pending &= ~sig_mask(signo);
                rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

                LOG_D("handle signal: %d, handler 0x%08x", signo, handler);
                if (handler) handler(signo);

                level = rt_spin_lock_irqsave(&_thread_signal_lock);
                error = -RT_EINTR;

                rt_mp_free(si_node); /* 节点在池内分配，须还回 _siginfo_pool */
                /* 与 POSIX 类似：信号打断阻塞系统调用时置 -RT_EINTR */
                tid->error = error;
            }

            /* whether clean signal status */
            if (clean_state == RT_TRUE)
            {
                RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL;
            }
            else
            {
                return;
            }
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
}

/* 为线程分配 sig_vectors 并填默认 handler；若并发已分配则释放本次 malloc 的副本 */
void rt_thread_alloc_sig(rt_thread_t tid)
{
    int index;
    rt_bool_t need_free = RT_FALSE;
    rt_base_t level;
    rt_sighandler_t *vectors;

    vectors = (rt_sighandler_t *)RT_KERNEL_MALLOC(sizeof(rt_sighandler_t) * RT_SIG_MAX);
    RT_ASSERT(vectors != RT_NULL);

    for (index = 0; index < RT_SIG_MAX; index ++)
    {
        vectors[index] = _signal_default_handler;
    }

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    if (tid->sig_vectors == RT_NULL)
    {
        tid->sig_vectors = vectors;
    }
    else
    {
        need_free = RT_TRUE;
    }

    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    if (need_free)
    {
        rt_free(vectors);
    }
}

/* 删除线程时释放 si_list 链与 sig_vectors，调用方应保证线程不再运行信号路径 */
void rt_thread_free_sig(rt_thread_t tid)
{
    rt_base_t level;
    struct siginfo_node *si_node;
    rt_sighandler_t *sig_vectors;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    si_node = (struct siginfo_node *)tid->si_list;
    tid->si_list = RT_NULL;

    sig_vectors = tid->sig_vectors;
    tid->sig_vectors = RT_NULL;
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    if (si_node)
    {
        struct rt_slist_node *node;
        struct rt_slist_node *node_to_free;

        LOG_D("free signal info list");
        node = &(si_node->list);
        do
        {
            node_to_free = node;
            node = node->next;
            si_node = rt_slist_entry(node_to_free, struct siginfo_node, list);
            rt_mp_free(si_node);
        } while (node);
    }

    if (sig_vectors)
    {
        RT_KERNEL_FREE(sig_vectors);
    }
}

/**
 * @brief    This function can be used to send any signal to any thread.
 *
 * @param    tid is a pointer to the thread that receives the signal.
 *
 * @param    sig is a specific signal value (range: 0 ~ RT_SIG_MAX).
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the signal send failed.
 * @note     中文：同号已 pending 时仅更新链上已有节点的 siginfo；新节点挂链后
 *           置 pending 位并 _signal_deliver。池耗尽返回 -RT_EEMPTY。
 */
int rt_thread_kill(rt_thread_t tid, int sig)
{
    siginfo_t si;
    rt_base_t level;
    struct siginfo_node *si_node;

    RT_ASSERT(tid != RT_NULL);
    if (!sig_valid(sig)) return -RT_EINVAL;

    LOG_I("send signal: %d", sig);
    si.si_signo = sig;
    si.si_code  = SI_USER;
    si.si_value.sival_ptr = RT_NULL;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_pending & sig_mask(sig))
    {
        /* whether already emits this signal? */
        struct rt_slist_node *node;
        struct siginfo_node  *entry;

        si_node = (struct siginfo_node *)tid->si_list;
        if (si_node)
            node = (struct rt_slist_node *)&si_node->list;
        else
            node = RT_NULL;

        /* update sig info */
        for (; (node) != RT_NULL; node = node->next)
        {
            entry = rt_slist_entry(node, struct siginfo_node, list);
            if (entry->si.si_signo == sig)
            {
                memcpy(&(entry->si), &si, sizeof(siginfo_t));
                rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
                return 0;
            }
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    si_node = (struct siginfo_node *) rt_mp_alloc(_siginfo_pool, 0);
    if (si_node)
    {
        rt_slist_init(&(si_node->list));
        memcpy(&(si_node->si), &si, sizeof(siginfo_t));

        level = rt_spin_lock_irqsave(&_thread_signal_lock);

        if (tid->si_list)
        {
            struct siginfo_node *si_list;

            si_list = (struct siginfo_node *)tid->si_list;
            rt_slist_append(&(si_list->list), &(si_node->list));
        }
        else
        {
            tid->si_list = si_node;
        }

        /* a new signal */
        tid->sig_pending |= sig_mask(sig);

        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    }
    else
    {
        LOG_E("The allocation of signal info node failed.");
        return -RT_EEMPTY;
    }

    /* deliver signal to this thread */
    _signal_deliver(tid);

    return RT_EOK;
}

/* 系统初始化阶段创建 siginfo 节点池，失败则断言 */
int rt_system_signal_init(void)
{
    _siginfo_pool = rt_mp_create("signal", RT_SIG_INFO_MAX, sizeof(struct siginfo_node));
    if (_siginfo_pool == RT_NULL)
    {
        LOG_E("create memory pool for signal info failed.");
        RT_ASSERT(0);
    }

    return 0;
}

#endif /* RT_USING_SIGNALS */
