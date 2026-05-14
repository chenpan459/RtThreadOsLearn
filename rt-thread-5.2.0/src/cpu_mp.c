/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-10-30     Bernard      The first version
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       spinlock should lock sched
 * 2024-01-25     Shell        Using rt_exit_critical_safe
 */

/*
 * 本文件职责（中文概要，仅 RT_USING_SMP 时编译）：
 * - 每 CPU 控制块数组 _cpus[]：保存当前线程、idle、tick、就绪队列等调度相关状态（与 scheduler_mp 配合）。
 * - 自旋锁封装 rt_spin_*：在持锁期间通过 rt_enter_critical 禁止抢占，避免同核死锁；irqsave 变体同时关本地中断。
 * - 全局大锁 rt_cpus_lock / rt_cpus_unlock：按「当前线程的 cpus_lock_nest 嵌套计数」串行化跨核临界区，用于需冻结全系统调度的场景。
 * - rt_cpu_self / rt_cpu_index / rt_cpu_get_id：CPU 抽象查询；底层 ID 来自 rt_hw_cpu_id()（libcpu 实现）。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_SMART
#include <lwp.h>
#endif

#ifdef RT_USING_DEBUG
/* 获取 _cpus_lock 时的调度器临界区嵌套深度，解锁时用于 rt_exit_critical_safe 恢复到一致状态 */
rt_base_t _cpus_critical_level;
#endif /* RT_USING_DEBUG */

/* SMP 下每个逻辑 CPU 一颗 rt_cpu，下标与 rt_hw_cpu_id() 一致 */
static struct rt_cpu _cpus[RT_CPUS_NR];
/* 全核调度冻结锁：与 rt_cpus_lock_nest 配合，仅首个嵌套获取时真正加锁 */
rt_hw_spinlock_t _cpus_lock;
#if defined(RT_DEBUGING_SPINLOCK)
void *_cpus_lock_owner = 0;
void *_cpus_lock_pc = 0;

#endif /* RT_DEBUGING_SPINLOCK */

/**
 * @brief   Initialize a static spinlock object.
 *
 * @param   lock is a pointer to the spinlock to initialize.
 * @note    封装底层 rt_hw_spin_lock_init，供静态分配的自旋锁在运行前初始化。
 */
void rt_spin_lock_init(struct rt_spinlock *lock)
{
    rt_hw_spin_lock_init(&lock->lock);
}
RTM_EXPORT(rt_spin_lock_init)

/**
 * @brief   This function will lock the spinlock, will lock the thread scheduler.
 *
 * @note    If the spinlock is locked, the current CPU will keep polling the spinlock state
 *          until the spinlock is unlocked.
 * @note    先进入调度临界区再自旋等待锁，防止本核在持锁等待时被抢占导致同核重入死锁。
 *
 * @param   lock is a pointer to the spinlock.
 */
void rt_spin_lock(struct rt_spinlock *lock)
{
    /* 禁止本核线程抢占 */
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
}
RTM_EXPORT(rt_spin_lock)

/**
 * @brief   This function will unlock the spinlock, will unlock the thread scheduler.
 *
 * @param   lock is a pointer to the spinlock.
 * @note    释放硬件自旋锁后按加锁前临界区状态安全退出（rt_exit_critical_safe）。
 */
void rt_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
}
RTM_EXPORT(rt_spin_unlock)

/**
 * @brief   This function will disable the local interrupt and then lock the spinlock, will lock the thread scheduler.
 *
 * @note    If the spinlock is locked, the current CPU will keep polling the spinlock state
 *          until the spinlock is unlocked.
 * @note    关本地中断 + 禁止抢占 + 自旋；适用于 ISR 与线程可能共享的数据，返回 level 供 irqrestore 配对。
 *
 * @param   lock is a pointer to the spinlock.
 *
 * @return  Return current cpu interrupt status.
 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level;

    level = rt_hw_local_irq_disable();
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
    return level;
}
RTM_EXPORT(rt_spin_lock_irqsave)

/**
 * @brief   This function will unlock the spinlock and then restore current cpu interrupt status, will unlock the thread scheduler.
 *
 * @param   lock is a pointer to the spinlock.
 *
 * @param   level is interrupt status returned by rt_spin_lock_irqsave().
 * @note    与 rt_spin_lock_irqsave 严格配对，先解锁再恢复中断使能状态。
 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level)
{
    rt_base_t critical_level;

    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
    rt_hw_local_irq_enable(level);
}
RTM_EXPORT(rt_spin_unlock_irqrestore)

/**
 * @brief   This fucntion will return current cpu object.
 *
 * @return  Return a pointer to the current cpu object.
 * @note    根据 rt_hw_cpu_id() 返回本核对应的 rt_cpu（含 current_thread、irq_nest 等）。
 */
struct rt_cpu *rt_cpu_self(void)
{
    return &_cpus[rt_hw_cpu_id()];
}

/**
 * @brief   This fucntion will return the cpu object corresponding to index.
 *
 * @param   index is the index of target cpu object.
 *
 * @return  Return a pointer to the cpu object corresponding to index.
 * @note    按逻辑 CPU 下标取 rt_cpu；index 须小于 RT_CPUS_NR。
 */
struct rt_cpu *rt_cpu_index(int index)
{
    return &_cpus[index];
}

/**
 * @brief   This function will lock all cpus's scheduler and disable local irq.
 *
 * @return  Return current cpu interrupt status.
 * @note    冻结「所有核」上的调度迁移（通过全局 _cpus_lock）；支持同一线程递归调用，仅最外层真正加锁。
 */
rt_base_t rt_cpus_lock(void)
{
    rt_base_t level;
    struct rt_cpu* pcpu;

    level = rt_hw_local_irq_disable();
    pcpu = rt_cpu_self();
    if (pcpu->current_thread != RT_NULL)
    {
        /* 加锁前读取旧嵌套计数，为 0 表示本线程首次进入全核临界区 */
        rt_ubase_t lock_nest = rt_atomic_load(&(pcpu->current_thread->cpus_lock_nest));

        rt_atomic_add(&(pcpu->current_thread->cpus_lock_nest), 1);
        if (lock_nest == 0)
        {
            rt_enter_critical();
            rt_hw_spin_lock(&_cpus_lock);
#ifdef RT_USING_DEBUG
            _cpus_critical_level = rt_critical_level();
#endif /* RT_USING_DEBUG */

#ifdef RT_DEBUGING_SPINLOCK
            _cpus_lock_owner = pcpu->current_thread;
            _cpus_lock_pc = __GET_RETURN_ADDRESS;
#endif /* RT_DEBUGING_SPINLOCK */
        }
    }

    return level;
}
RTM_EXPORT(rt_cpus_lock);

/**
 * @brief   This function will restore all cpus's scheduler and restore local irq.
 *
 * @param   level is interrupt status returned by rt_cpus_lock().
 * @note    与 rt_cpus_lock 配对；嵌套计数归零时才释放 _cpus_lock 并恢复调度临界区。
 */
void rt_cpus_unlock(rt_base_t level)
{
    struct rt_cpu* pcpu = rt_cpu_self();

    if (pcpu->current_thread != RT_NULL)
    {
        rt_base_t critical_level = 0;
        RT_ASSERT(rt_atomic_load(&(pcpu->current_thread->cpus_lock_nest)) > 0);
        rt_atomic_sub(&(pcpu->current_thread->cpus_lock_nest), 1);

        /* 最外层配对：释放全核自旋锁并按加锁前记录的 critical_level 退出 */
        if (pcpu->current_thread->cpus_lock_nest == 0)
        {
#if defined(RT_DEBUGING_SPINLOCK)
            _cpus_lock_owner = __OWNER_MAGIC;
            _cpus_lock_pc = RT_NULL;
#endif /* RT_DEBUGING_SPINLOCK */
#ifdef RT_USING_DEBUG
            critical_level = _cpus_critical_level;
            _cpus_critical_level = 0;
#endif /* RT_USING_DEBUG */
            rt_hw_spin_unlock(&_cpus_lock);
            rt_exit_critical_safe(critical_level);
        }
    }
    /* 无论是否持有 _cpus_lock，均在最后恢复调用 rt_cpus_lock 前保存的中断状态 */
    rt_hw_local_irq_enable(level);
}
RTM_EXPORT(rt_cpus_unlock);

/**
 * This function is invoked by scheduler.
 * It will restore the lock state to whatever the thread's counter expects.
 * If target thread not locked the cpus then unlock the cpus lock.
 *
 * @param   thread is a pointer to the target thread.
 * @note    上下文切换后由调度器调用；在 SMART+MMU 下切换地址空间，再投递调度后处理（与 cpus_lock_nest 状态对齐）。
 */
void rt_cpus_lock_status_restore(struct rt_thread *thread)
{
#if defined(ARCH_MM_MMU) && defined(RT_USING_SMART)
    lwp_aspace_switch(thread);
#endif
    rt_sched_post_ctx_switch(thread);
}
RTM_EXPORT(rt_cpus_lock_status_restore);

/* A safe API with debugging feature to be called in most codes */

#undef rt_cpu_get_id
/**
 * @brief Get logical CPU ID
 *
 * @return logical CPU ID
 * @note    仅在「已绑核 / 关中断 / 调度器不可用」之一成立时允许调用，避免在可抢占迁移点误读 CPU ID。
 */
rt_base_t rt_cpu_get_id(void)
{

    RT_ASSERT(rt_sched_thread_is_binding(RT_NULL) ||
              rt_hw_interrupt_is_disabled() ||
              !rt_scheduler_is_available());

    return rt_hw_cpu_id();
}
