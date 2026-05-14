/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-09-20     Bernard      Change the name to components.c
 *                             And all components related header files.
 * 2012-12-23     Bernard      fix the pthread initialization issue.
 * 2013-06-23     Bernard      Add the init_call for components initialization.
 * 2013-07-05     Bernard      Remove initialization feature for MS VC++ compiler
 * 2015-02-06     Bernard      Remove the MS VC++ support and move to the kernel
 * 2015-05-04     Bernard      Rename it to components.c because compiling issue
 *                             in some IDEs.
 * 2015-07-29     Arda.Fu      Add support to use RT_USING_USER_MAIN with IAR
 * 2018-11-22     Jesven       Add secondary cpu boot up
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 */

/*
 * 本文件职责（中文概要）：
 * 1) 在 RT_USING_COMPONENTS_INIT 下：通过链接脚本收集的初始化函数表，按阶段执行板级与组件自动初始化。
 * 2) 在 RT_USING_USER_MAIN 下：提供各工具链的 C 入口包装，以及 rtthread_startup() 的完整内核启动序列，
 *    并创建 main 线程，在其中调用 rt_components_init() 与用户 main()。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_USER_MAIN
/* main 线程栈大小（字节），可在 rtconfig.h 中覆盖 RT_MAIN_THREAD_STACK_SIZE */
#ifndef RT_MAIN_THREAD_STACK_SIZE
#define RT_MAIN_THREAD_STACK_SIZE     2048
#endif /* RT_MAIN_THREAD_STACK_SIZE */
/* main 线程优先级，默认同优先级档位的约 1/3（数值越大优先级越低时取决于 RT_THREAD_PRIORITY_MAX 语义） */
#ifndef RT_MAIN_THREAD_PRIORITY
#define RT_MAIN_THREAD_PRIORITY       (RT_THREAD_PRIORITY_MAX / 3)
#endif /* RT_MAIN_THREAD_PRIORITY */
#endif /* RT_USING_USER_MAIN */

#ifdef RT_USING_COMPONENTS_INIT
/*
 * Components Initialization will initialize some driver and components as following
 * order:
 * （中文）组件自动初始化按以下阶段顺序执行，各阶段由 INIT_*_EXPORT 注册的函数指针经链接器排序后依次调用：
 * rti_start         --> 0
 * BOARD_EXPORT      --> 1
 * rti_board_end     --> 1.end
 *
 * DEVICE_EXPORT     --> 2
 * COMPONENT_EXPORT  --> 3
 * FS_EXPORT         --> 4
 * ENV_EXPORT        --> 5
 * APP_EXPORT        --> 6
 *
 * rti_end           --> 6.end
 *
 * These automatically initialization, the driver or component initial function must
 * be defined with:
 * INIT_BOARD_EXPORT(fn);
 * INIT_DEVICE_EXPORT(fn);
 * ...
 * INIT_APP_EXPORT(fn);
 * etc.
 *
 * 下面 rti_* 占位函数仅用于界定链接器生成的 __rt_init_* 符号区间，本身无业务逻辑。
 */
static int rti_start(void)
{
    return 0;
}
INIT_EXPORT(rti_start, "0");

static int rti_board_start(void)
{
    return 0;
}
INIT_EXPORT(rti_board_start, "0.end");

static int rti_board_end(void)
{
    return 0;
}
INIT_EXPORT(rti_board_end, "1.end");

static int rti_end(void)
{
    return 0;
}
INIT_EXPORT(rti_end, "6.end");

/**
 * @brief  Onboard components initialization. In this function, the board-level
 *         initialization function will be called to complete the initialization
 *         of the on-board peripherals.
 * @note 板级组件初始化，仅调用 INIT_BOARD_EXPORT 等「板级段」函数，
 *       用于时钟、引脚、早期串口等，早于设备框架与文件系统。
 */
void rt_components_board_init(void)
{
#ifdef RT_DEBUGING_AUTO_INIT
    /* 调试：打印每个初始化函数名与返回值，便于排查自动初始化顺序问题 */
    int result;
    const struct rt_init_desc *desc;
    for (desc = &__rt_init_desc_rti_board_start; desc < &__rt_init_desc_rti_board_end; desc ++)
    {
        rt_kprintf("initialize %s\n", desc->fn_name);
        result = desc->fn();
        rt_kprintf(":%d done\n", result);
    }
#else
    /* 非调试模式：仅函数指针，无函数名字符串，体积更小 */
    volatile const init_fn_t *fn_ptr;

    for (fn_ptr = &__rt_init_rti_board_start; fn_ptr < &__rt_init_rti_board_end; fn_ptr++)
    {
        (*fn_ptr)();
    }
#endif /* RT_DEBUGING_AUTO_INIT */
}

/**
 * @brief  RT-Thread Components Initialization.
 * @note 从板级段结束之后到 rti_end 之前，依次执行设备、组件、文件系统等阶段。
 */
void rt_components_init(void)
{
#ifdef RT_DEBUGING_AUTO_INIT
    /* 调试：逐条打印组件初始化函数名与返回值 */
    int result;
    const struct rt_init_desc *desc;

    rt_kprintf("do components initialization.\n");
    for (desc = &__rt_init_desc_rti_board_end; desc < &__rt_init_desc_rti_end; desc ++)
    {
        rt_kprintf("initialize %s\n", desc->fn_name);
        result = desc->fn();
        rt_kprintf(":%d done\n", result);
    }
#else
    volatile const init_fn_t *fn_ptr;

    for (fn_ptr = &__rt_init_rti_board_end; fn_ptr < &__rt_init_rti_end; fn_ptr ++)
    {
        (*fn_ptr)();
    }
#endif /* RT_DEBUGING_AUTO_INIT */
}
#endif /* RT_USING_COMPONENTS_INIT */

#ifdef RT_USING_USER_MAIN

void rt_application_init(void);
void rt_hw_board_init(void);
int rtthread_startup(void);

#ifdef __ARMCC_VERSION
extern int $Super$$main(void);
/* ARM Compiler 6/5：用 $Sub$$main 包装用户 main，先进入 RT-Thread 启动再链到 $Super$$main */
/* re-define main function */
int $Sub$$main(void)
{
    rtthread_startup();
    return 0;
}
#elif defined(__ICCARM__)
/* IAR：在 cstartup 中自动调用 __low_level_init，需先完成数据段拷贝再启动内核 */
/* __low_level_init will auto called by IAR cstartup */
extern void __iar_data_init3(void);
int __low_level_init(void)
{
    /* 调用 IAR 运行时表拷贝，再进入 RT-Thread */
    __iar_data_init3();
    rtthread_startup();
    return 0;
}
#elif defined(__GNUC__)
/* GCC：链接时使用 -eentry 将入口设为 entry()，而非默认 _start 后直接到 main */
/* Add -eentry to arm-none-eabi-gcc argument */
int entry(void)
{
    rtthread_startup();
    return 0;
}
#endif

#ifndef RT_USING_HEAP
/* 未使能堆时：main 线程必须用静态线程控制块与静态栈，不能使用 rt_thread_create */
/* if there is not enable heap, we should use static thread and stack. */
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t main_thread_stack[RT_MAIN_THREAD_STACK_SIZE];
struct rt_thread main_thread;
#endif /* RT_USING_HEAP */

/**
 * @brief  The system main thread. In this thread will call the rt_components_init()
 *         for initialization of RT-Thread Components and call the user's programming
 *         entry main().
 * @note 系统 main 线程入口；先组件初始化，SMP 时拉起从核，最后调用用户 main()。
 *
 * @param  parameter is the arg of the thread.
 */
static void main_thread_entry(void *parameter)
{
    extern int main(void);
    RT_UNUSED(parameter);

#ifdef RT_USING_COMPONENTS_INIT
    /* 设备/文件系统/网络等 INIT_DEVICE_EXPORT 及之后阶段的初始化 */
    /* RT-Thread components initialization */
    rt_components_init();
#endif /* RT_USING_COMPONENTS_INIT */

#ifdef RT_USING_SMP
    /* 主核就绪后，按平台实现唤醒其余 CPU 核 */
    rt_hw_secondary_cpu_up();
#endif /* RT_USING_SMP */
    /* 进入用户应用入口（与工具链相关的 main 符号） */
    /* invoke system main function */
#ifdef __ARMCC_VERSION
    {
        extern int $Super$$main(void);
        $Super$$main(); /* for ARMCC. */
    }
#elif defined(__ICCARM__) || defined(__GNUC__) || defined(__TASKING__) || defined(__TI_COMPILER_VERSION__)
    main();
#endif /* __ARMCC_VERSION */
}

/**
 * @brief  This function will create and start the main thread, but this thread
 *         will not run until the scheduler starts.
 * @note 创建并启动 "main" 线程，此处仅入就绪队列，真正运行要等 rt_system_scheduler_start()。
 */
void rt_application_init(void)
{
    rt_thread_t tid;

#ifdef RT_USING_HEAP
    tid = rt_thread_create("main", main_thread_entry, RT_NULL,
                           RT_MAIN_THREAD_STACK_SIZE, RT_MAIN_THREAD_PRIORITY, 20);
    RT_ASSERT(tid != RT_NULL);
#else
    rt_err_t result;

    tid = &main_thread;
    result = rt_thread_init(tid, "main", main_thread_entry, RT_NULL,
                            main_thread_stack, sizeof(main_thread_stack), RT_MAIN_THREAD_PRIORITY, 20);
    RT_ASSERT(result == RT_EOK);

    /* if not define RT_USING_HEAP, using to eliminate the warning */
    (void)result;
#endif /* RT_USING_HEAP */

    rt_thread_startup(tid);
}

/**
 * @brief  This function will call all levels of initialization functions to complete
 *         the initialization of the system, and finally start the scheduler.
 * @note 内核冷启动总入口；板级与子系统初始化后创建 main/定时器/idle 等线程，再启动调度器。
 *       正常在 rt_system_scheduler_start() 切到首个线程后不会返回。
 *
 * @return Normally never returns. If 0 is returned, the scheduler failed.
 */
int rtthread_startup(void)
{
#ifdef RT_USING_SMP
    /* 多核全局自旋锁，供内核早期与调度路径使用 */
    rt_hw_spin_lock_init(&_cpus_lock);
#endif
    /* 启动早期关闭本地中断，避免初始化过程被抢占 */
    rt_hw_local_irq_disable();

    /* board level initialization
     * NOTE: please initialize heap inside board initialization.
     */
    /* 板级初始化：时钟、堆、控制台等；若使用动态创建线程，须在此完成 rt_system_heap_init */
    rt_hw_board_init();

    /* show RT-Thread version */
    /* 打印内核版本信息（通常经串口） */
    rt_show_version();

    /* timer system initialization */
    /* 软件定时器模块数据结构初始化（尚无线程上下文） */
    rt_system_timer_init();

    /* scheduler system initialization */
    /* 就绪队列、优先级位图等调度器内部状态初始化 */
    rt_system_scheduler_init();

#ifdef RT_USING_SIGNALS
    /* signal system initialization */
    /* 可选：线程信号子系统 */
    rt_system_signal_init();
#endif /* RT_USING_SIGNALS */

    /* create init_thread */
    /* 创建 main 线程（main_thread_entry），此时仅就绪，未占用 CPU */
    rt_application_init();

    /* timer thread initialization */
    /* 创建用于处理定时器超时的内核服务线程 */
    rt_system_timer_thread_init();

    /* idle thread initialization */
    /* 创建 idle 线程（每 CPU 一份，用于空闲与回收等后台工作） */
    rt_thread_idle_init();

    /* defunct thread initialization */
    /* 已退出线程的延迟销毁队列相关初始化（与 SMP/SMART 等配置相关） */
    rt_thread_defunct_init();

#ifdef RT_USING_SMP
    rt_hw_spin_lock(&_cpus_lock);
#endif /* RT_USING_SMP */

    /* start scheduler */
    /* 选出最高优先级就绪线程并首次上下文切换；之后系统在线程间运行 */
    rt_system_scheduler_start();

    /* never reach here */
    /* 调度器正常启动后不会执行到此处 */
    return 0;
}
#endif /* RT_USING_USER_MAIN */
