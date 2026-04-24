/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-04-19     Shell        Fixup UP irq spinlock
 * 2024-05-22     Shell        Add UP cpu object and
 *                             maintain the rt_current_thread inside it
 */

/*
 * 本文件职责（中文概要，单核 RT_USING_SMP 关闭时编译，与 cpu_mp.c 二选一）：
 * - 提供唯一的 struct rt_cpu 实例 _cpu，与 SMP 下每核 _cpus[] 接口兼容（rt_cpu_self/rt_cpu_index）。
 * - 自旋锁 API 在单核上退化为「调度器临界区 + 可选关中断」：无真正的多核自旋等待，避免 UP 下无意义的硬件 spin。
 * - index 非 0 时 rt_cpu_index 返回 RT_NULL，表示不存在其它逻辑 CPU。
 */

#include <rthw.h>
#include <rtthread.h>

/* 单核全局唯一的 CPU 控制块（current_thread、tick、idle 等均挂在此结构上，与 scheduler_up 一致） */
static struct rt_cpu _cpu;

/**
 * @brief   Initialize a static spinlock object.
 *
 * @param   lock is a pointer to the spinlock to initialize.
 * @note    中文：UP 无硬件自旋语义，空操作即可保持与 MP 相同的调用点。
 */
void rt_spin_lock_init(struct rt_spinlock *lock)
{
    RT_UNUSED(lock);
}

/**
 * @brief   This function will lock the spinlock, will lock the thread scheduler.
 *
 * @note    If the spinlock is locked, the current CPU will keep polling the spinlock state
 *          until the spinlock is unlocked.
 * @note    中文：等价于禁止线程抢占（rt_enter_critical），无跨核自旋；与 MP 版 API 形态一致便于共用代码。
 *
 * @param   lock is a pointer to the spinlock.
 */
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
}

/**
 * @brief   This function will unlock the spinlock, will unlock the thread scheduler.
 *
 * @param   lock is a pointer to the spinlock.
 * @note    中文：按调试信息记录的临界区嵌套安全退出，不操作硬件 spin。
 */
void rt_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
}

/**
 * @brief   This function will disable the local interrupt and then lock the spinlock, will lock the thread scheduler.
 *
 * @note    If the spinlock is locked, the current CPU will keep polling the spinlock state
 *          until the spinlock is unlocked.
 * @note    中文：关中断 + 禁止抢占；返回 level 供 irqrestore 配对（UP 下常用作短临界区保护）。
 *
 * @param   lock is a pointer to the spinlock.
 *
 * @return  Return current cpu interrupt status.
 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level;
    RT_UNUSED(lock);
    /* 注意：UP 使用 rt_hw_interrupt_disable（与部分 MP 路径的 local_irq 命名不同），与 BSP 提供的关中断实现对应 */
    level = rt_hw_interrupt_disable();
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
    return level;
}

/**
 * @brief   This function will unlock the spinlock and then restore current cpu interrupt status, will unlock the thread scheduler.
 *
 * @param   lock is a pointer to the spinlock.
 *
 * @param   level is interrupt status returned by rt_spin_lock_irqsave().
 * @note    中文：先退出调度临界区再恢复中断，与 rt_spin_lock_irqsave 严格配对。
 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
    rt_hw_interrupt_enable(level);
}

/**
 * @brief   This fucntion will return current cpu object.
 *
 * @return  Return a pointer to the current cpu object.
 * @note    中文：恒为 &_cpu，与 SMP 下按 rt_hw_cpu_id() 索引的行为对应。
 */
struct rt_cpu *rt_cpu_self(void)
{
    return &_cpu;
}

/**
 * @brief   This fucntion will return the cpu object corresponding to index.
 *
 * @param   index is the index of target cpu object.
 *
 * @return  Return a pointer to the cpu object corresponding to index.
 * @note    中文：仅 index==0 合法；其它下标返回 RT_NULL，表示无多核。
 */
struct rt_cpu *rt_cpu_index(int index)
{
    return index == 0 ? &_cpu : RT_NULL;
}
