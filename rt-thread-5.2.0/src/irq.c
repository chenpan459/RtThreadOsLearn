/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-02-24     Bernard      first version
 * 2006-05-03     Bernard      add IRQ_DEBUG
 * 2016-08-09     ArdaFu       add interrupt enter and leave hook.
 * 2018-11-22     Jesven       rt_interrupt_get_nest function add disable irq
 * 2021-08-15     Supperthomas fix the comment
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to irq.c
 * 2022-07-04     Yunjie       fix RT_DEBUG_LOG
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2024-01-05     Shell        Fixup of data racing in rt_interrupt_get_nest
 * 2024-01-03     Shell        Support for interrupt context
 *
 * ---------------------------------------------------------------------------
 * 模块说明（中断嵌套与上下文）
 * ---------------------------------------------------------------------------
 * 本文件维护「当前 CPU 处于 ISR 的嵌套深度」rt_interrupt_nest：每进一层硬件
 * 中断服务例程（或 BSP 约定的等价入口）调用一次 rt_interrupt_enter()，退出前
 * 对称调用 rt_interrupt_leave()。应用层不应直接调用二者，应由 BSP/移植层在
 * 统一中断入口/出口里配对调用，否则调度器对「是否在中断上下文」的判断会失真。
 *
 * SMP：嵌套计数放在 per-cpu 结构 rt_cpu->irq_nest，避免多核互相踩踏。
 * 读嵌套深度请用 rt_interrupt_get_nest()：内部关本地中断后读原子，避免与
 * enter/leave 并发时的撕裂读（Shell 2024 数据竞争修复）。
 *
 * ARCH_USING_IRQ_CTX_LIST：在嵌套中断时维护一条当前 CPU 上的「中断上下文」
 * 链表，便于回溯、调试或取最内层保存的 CPU 寄存器快照等；push/pop 与 ISR
 * 进入/退出顺序一致，由 BSP 或 arch 层与 rt_interrupt_enter/leave 协同调用。
 */

#include <rthw.h>
#include <rtthread.h>

#define DBG_TAG           "kernel.irq"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)

/* 进入/离开 ISR 时各调用一次的函数指针；须短小、不可阻塞或挂起当前上下文 */
static void (*rt_interrupt_enter_hook)(void);
static void (*rt_interrupt_leave_hook)(void);

/**
 * @ingroup group_Hook
 *
 * @brief This function set a hook function when the system enter a interrupt
 *
 * @note The hook function must be simple and never be blocked or suspend.
 * @note 在每次 rt_interrupt_enter() 末尾通过 RT_OBJECT_HOOK_CALL 触发。
 *       典型用途为统计、trace；禁止 malloc、信号量 take 等可能阻塞的操作。
 *
 * @param hook the function pointer to be called
 */
void rt_interrupt_enter_sethook(void (*hook)(void))
{
    rt_interrupt_enter_hook = hook;
}

/**
 * @ingroup group_Hook
 *
 * @brief This function set a hook function when the system exit a interrupt.
 *
 * @note The hook function must be simple and never be blocked or suspend.
 * @note 在 rt_interrupt_leave() 里先于 rt_atomic_sub(nest) 调用，与 enter
 *       侧 hook 成对；同样须 ISR 安全。
 *
 * @param hook the function pointer to be called
 */
void rt_interrupt_leave_sethook(void (*hook)(void))
{
    rt_interrupt_leave_hook = hook;
}
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_Kernel
 */

/**@{*/

#ifdef RT_USING_SMP
/* 每颗 CPU 独立的嵌套计数，访问时隐含在「当前核」上 */
#define rt_interrupt_nest rt_cpu_self()->irq_nest
#else
volatile rt_atomic_t rt_interrupt_nest = 0;
#endif /* RT_USING_SMP */

#ifdef ARCH_USING_IRQ_CTX_LIST
/* 最内层 ctx 在链表头；push 与 ISR 进入顺序一致，pop 与离开最外层一致 */
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx)
{
    struct rt_cpu *this_cpu = rt_cpu_self();
    rt_slist_insert(&this_cpu->irq_ctx_head, &this_ctx->node);
}

void rt_interrupt_context_pop(void)
{
    struct rt_cpu *this_cpu = rt_cpu_self();
    rt_slist_pop(&this_cpu->irq_ctx_head);
}

void *rt_interrupt_context_get(void)
{
    struct rt_cpu *this_cpu = rt_cpu_self();
    /* 假定已在 ISR 中且链表非空；由 arch/BSP 保证与 push 配对 */
    return rt_slist_first_entry(&this_cpu->irq_ctx_head, struct rt_interrupt_context, node)->context;
}
#endif /* ARCH_USING_IRQ_CTX_LIST */

/**
 * @brief This function will be invoked by BSP, when enter interrupt service routine
 *
 * @note Please don't invoke this routine in application
 * @note 原子自增嵌套计数；必须与 rt_interrupt_leave 严格配对（含嵌套中断
 *       路径）。调度、部分 IPC 会依据 rt_interrupt_get_nest() 判定是否在中断里。
 *
 * @see rt_interrupt_leave
 */
rt_weak void rt_interrupt_enter(void)
{
    rt_atomic_add(&(rt_interrupt_nest), 1);
    RT_OBJECT_HOOK_CALL(rt_interrupt_enter_hook,());
    LOG_D("irq has come..., irq current nest:%d",
          (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
}
RTM_EXPORT(rt_interrupt_enter);


/**
 * @brief This function will be invoked by BSP, when leave interrupt service routine
 *
 * @note Please don't invoke this routine in application
 * @note 先 leave hook 再减计数，与 enter 侧「先加计数再 hook」顺序对称；
 *       若配对错误可能导致嵌套计数永不为 0，进而误判始终在中断上下文。
 *
 * @see rt_interrupt_enter
 */
rt_weak void rt_interrupt_leave(void)
{
    LOG_D("irq is going to leave, irq current nest:%d",
                 (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
    RT_OBJECT_HOOK_CALL(rt_interrupt_leave_hook,());
    rt_atomic_sub(&(rt_interrupt_nest), 1);

}
RTM_EXPORT(rt_interrupt_leave);


/**
 * @brief This function will return the nest of interrupt.
 *
 * User application can invoke this function to get whether current
 * context is interrupt context.
 *
 * @return the number of nested interrupts.
 * @note 返回 0 表示线程/非 ISR 上下文；大于 0 表示处于中断（含嵌套层数）。
 *       关本地 IRQ 后再读原子，避免与 enter/leave 交错读到不一致的中间状态。
 */
rt_weak rt_uint8_t rt_interrupt_get_nest(void)
{
    rt_uint8_t ret;
    rt_base_t level;

    level = rt_hw_local_irq_disable();
    ret = rt_atomic_load(&rt_interrupt_nest);
    rt_hw_local_irq_enable(level);
    return ret;
}
RTM_EXPORT(rt_interrupt_get_nest);

/* 导出到 Finsh/模块符号表；具体实现在各 BSP/arch 的汇编或 rthw 移植层 */
RTM_EXPORT(rt_hw_interrupt_disable);
RTM_EXPORT(rt_hw_interrupt_enable);

/**
 * @brief Query whether local IRQ is currently masked (weak default).
 *
 * @note 默认恒为 RT_FALSE；若 SoC/移植层能区分「全局关中断」状态，可
 *       rt_weak 覆盖本函数供断言或调试使用。
 */
rt_weak rt_bool_t rt_hw_interrupt_is_disabled(void)
{
    return RT_FALSE;
}
RTM_EXPORT(rt_hw_interrupt_is_disabled);
/**@}*/

