/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-23     Bernard      the first version
 * 2010-11-10     Bernard      add cleanup callback function in thread exit.
 * 2012-12-29     Bernard      fix compiling warning.
 * 2013-12-21     Grissiom     let rt_thread_idle_excute loop until there is no
 *                             dead thread.
 * 2016-08-09     ArdaFu       add method to get the handler of the idle thread.
 * 2018-02-07     Bernard      lock scheduler to protect tid->cleanup.
 * 2018-07-14     armink       add idle hook list
 * 2018-11-22     Jesven       add per cpu idle task
 *                             combine the code of primary and secondary cpu
 * 2021-11-15     THEWON       Remove duplicate work between idle and _thread_exit
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-11-07     xqyjlj       fix thread exit
 * 2023-12-10     xqyjlj       add _hook_spinlock
 */

/*
 * 本文件职责（中文概要）：
 * - 为每个逻辑 CPU 创建最低优先级的 idle 线程（tidle0…），无其它就绪线程时运行。
 * - 在 idle 循环中可选执行 idle hook、电源管理；单核或非 SMART 下调用 rt_defunct_execute() 回收已退出线程。
 * - SMP：从核 idle 进入 rt_hw_secondary_cpu_idle_exec()；主核仍走通用循环；各核 idle 与 rt_cpu->idle_thread 绑定。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MODULE
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

#ifdef RT_USING_HOOK
#ifndef RT_USING_IDLE_HOOK
#define RT_USING_IDLE_HOOK
#endif /* RT_USING_IDLE_HOOK */
#endif /* RT_USING_HOOK */

#ifndef IDLE_THREAD_STACK_SIZE
/* 有 idle hook 或堆时 idle 栈略大，便于在 hook 里做稍多工作 */
#if defined (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP)
#define IDLE_THREAD_STACK_SIZE  256
#else
#define IDLE_THREAD_STACK_SIZE  128
#endif /* (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP) */
#endif /* IDLE_THREAD_STACK_SIZE */

#define _CPUS_NR                RT_CPUS_NR

/* 每颗 CPU 一个 idle 线程及其栈（UP 时仅 [0] 有效） */
static struct rt_thread idle_thread[_CPUS_NR];
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t idle_thread_stack[_CPUS_NR][IDLE_THREAD_STACK_SIZE];

#ifdef RT_USING_IDLE_HOOK
#ifndef RT_IDLE_HOOK_LIST_SIZE
#define RT_IDLE_HOOK_LIST_SIZE  4
#endif /* RT_IDLE_HOOK_LIST_SIZE */

static void (*idle_hook_list[RT_IDLE_HOOK_LIST_SIZE])(void);
static struct rt_spinlock _hook_spinlock;

/**
 * @brief This function sets a hook function to idle thread loop. When the system performs
 *        idle loop, this hook function should be invoked.
 *
 * @param hook the specified hook function.
 *
 * @return RT_EOK: set OK.
 *         -RT_EFULL: hook list is full.
 *
 * @note the hook function must be simple and never be blocked or suspend.
 * @note 在空闲链表中注册回调，须短小不可阻塞；与 delhook 共用 _hook_spinlock 保护表项。
 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void))
{
    rt_size_t i;
    rt_err_t ret = -RT_EFULL;
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_hook_spinlock);

    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        if (idle_hook_list[i] == RT_NULL)
        {
            idle_hook_list[i] = hook;
            ret = RT_EOK;
            break;
        }
    }

    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    return ret;
}

/**
 * @brief delete the idle hook on hook list.
 *
 * @param hook the specified hook function.
 *
 * @return RT_EOK: delete OK.
 *         -RT_ENOSYS: hook was not found.
 * @note 从固定大小表中移除指定指针；未找到返回 -RT_ENOSYS。
 */
rt_err_t rt_thread_idle_delhook(void (*hook)(void))
{
    rt_size_t i;
    rt_err_t ret = -RT_ENOSYS;
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_hook_spinlock);

    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        if (idle_hook_list[i] == hook)
        {
            idle_hook_list[i] = RT_NULL;
            ret = RT_EOK;
            break;
        }
    }

    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    return ret;
}

#endif /* RT_USING_IDLE_HOOK */

static void idle_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
#ifdef RT_USING_SMP
    /* 从核：不跑主核的 hook/defunct 路径，由平台实现 WFI/自旋等低功耗或让步 */
    if (rt_cpu_get_id() != 0)
    {
        while (1)
        {
            rt_hw_secondary_cpu_idle_exec();
        }
    }
#endif /* RT_USING_SMP */

    /* 主核（或 UP 唯一核）：标准 idle 循环 */
    while (1)
    {
#ifdef RT_USING_IDLE_HOOK
        rt_size_t i;
        void (*idle_hook)(void);

        for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
        {
            idle_hook = idle_hook_list[i];
            if (idle_hook != RT_NULL)
            {
                idle_hook();
            }
        }
#endif /* RT_USING_IDLE_HOOK */

#if !defined(RT_USING_SMP) && !defined(RT_USING_SMART)
    /* 单核且无 SMART：在 idle 里消费延迟销毁队列（SMP/SMART 由 defunct.c 的 tsystem 线程负责） */
    rt_defunct_execute();
#endif

#ifdef RT_USING_PM
        void rt_system_power_manager(void);
        /* 电源管理模块：可在空闲时进入更低功耗状态 */
        rt_system_power_manager();
#endif /* RT_USING_PM */
    }
}

/**
 * @brief This function will initialize idle thread, then start it.
 *
 * @note this function must be invoked when system init.
 * @note 为每个 CPU 创建优先级为 RT_THREAD_PRIORITY_MAX-1 的 idle，写入 rt_cpu_index(i)->idle_thread 并启动。
 */
void rt_thread_idle_init(void)
{
    rt_ubase_t i;
#if RT_NAME_MAX > 0
    char idle_thread_name[RT_NAME_MAX];
#endif /* RT_NAME_MAX > 0 */

#ifdef RT_USING_IDLE_HOOK
    rt_spin_lock_init(&_hook_spinlock);
#endif

    for (i = 0; i < _CPUS_NR; i++)
    {
#if RT_NAME_MAX > 0
        rt_snprintf(idle_thread_name, RT_NAME_MAX, "tidle%d", i);
#endif /* RT_NAME_MAX > 0 */
        /* 优先级最低档减 1，保证 idle 仅在没有其它就绪线程时运行 */
        rt_thread_init(&idle_thread[i],
#if RT_NAME_MAX > 0
                idle_thread_name,
#else
                "tidle",
#endif /* RT_NAME_MAX > 0 */
                idle_thread_entry,
                RT_NULL,
                &idle_thread_stack[i][0],
                sizeof(idle_thread_stack[i]),
                RT_THREAD_PRIORITY_MAX - 1,
                32);
#ifdef RT_USING_SMP
        /* 每个 idle 固定跑在对应物理/逻辑核上 */
        rt_thread_control(&idle_thread[i], RT_THREAD_CTRL_BIND_CPU, (void*)i);
#endif /* RT_USING_SMP */

        /* update */
        /* 供调度器与统计模块快速取得本 CPU 的 idle 线程指针 */
        rt_cpu_index(i)->idle_thread = &idle_thread[i];

        /* startup */
        rt_thread_startup(&idle_thread[i]);
    }
}

/**
 * @brief This function will get the handler of the idle thread.
 * @note 内部调用 rt_cpu_get_id()，须满足其调用约束（绑核/关中断/调度未就绪等之一）。
 */
rt_thread_t rt_thread_idle_gethandler(void)
{
    int id = rt_cpu_get_id();

    return (rt_thread_t)(&idle_thread[id]);
}
