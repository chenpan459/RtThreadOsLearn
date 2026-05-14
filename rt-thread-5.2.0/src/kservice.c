/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-16     Bernard      the first version
 * 2006-05-25     Bernard      rewrite vsprintf
 * 2006-08-10     Bernard      add rt_show_version
 * 2010-03-17     Bernard      remove rt_strlcpy function
 *                             fix gcc compiling issue.
 * 2010-04-15     Bernard      remove weak definition on ICCM16C compiler
 * 2012-07-18     Arda         add the alignment display for signed integer
 * 2012-11-23     Bernard      fix IAR compiler error.
 * 2012-12-22     Bernard      fix rt_kprintf issue, which found by Grissiom.
 * 2013-06-24     Bernard      remove rt_kprintf if RT_USING_CONSOLE is not defined.
 * 2013-09-24     aozima       make sure the device is in STREAM mode when used by rt_kprintf.
 * 2015-07-06     Bernard      Add rt_assert_handler routine.
 * 2021-02-28     Meco Man     add RT_KSERVICE_USING_STDLIB
 * 2021-12-20     Meco Man     implement rt_strcpy()
 * 2022-01-07     Gabriel      add __on_rt_assert_hook
 * 2022-06-04     Meco Man     remove strnlen
 * 2022-08-24     Yunjie       make rt_memset word-independent to adapt to ti c28x (16bit word)
 * 2022-08-30     Yunjie       make rt_vsnprintf adapt to ti c28x (16bit int)
 * 2023-02-02     Bernard      add Smart ID for logo version show
 * 2023-10-16     Shell        Add hook point for rt_malloc services
 * 2023-10-21     Shell        support the common backtrace API which is arch-independent
 * 2023-12-10     xqyjlj       perf rt_hw_interrupt_disable/enable, fix memheap lock
 * 2024-03-10     Meco Man     move std libc related functions to rtklibc
 *
 * ---------------------------------------------------------------------------
 * 模块说明（内核公共服务 kservice）
 * ---------------------------------------------------------------------------
 * 本文件集中实现与「板级弱符号、控制台输出、栈回溯、系统堆、位扫描、断言」
 * 相关的内核侧服务；字符串/内存等可复用例程已迁到 rtklibc，此处不再重复。
 *
 * - BSP 弱接口：延时、复位、关机、控制台字符输出、回溯帧读写、CPU 架构名等，
 *   默认打日志或空实现，由具体 BSP/arch 覆盖。
 * - 控制台：在 RT_USING_CONSOLE 下提供 rt_kprintf/rt_kputs、可选设备重定向与
 *   RT_USING_THREADSAFE_PRINTF（自旋锁 + 临界区）下的互斥打印，避免多线程
 *   与设备 write 交错乱序。
 * - 回溯：与 cpuport.h 中 RT_HW_BACKTRACE_* 宏及 rt_hw_backtrace_frame_* 协作；
 *   默认弱实现仅打印 PC 链提示用 addr2line。
 * - 系统堆：在 RT_USING_HEAP 且非 RT_USING_USERHEAP 时，按配置选用 small/memheap/
 *   slab 之一，并用 RT_USING_HEAP_ISR（自旋锁）或 RT_USING_MUTEX 或临界区保护
 *   rt_malloc/rt_free 等；支持 malloc/free/realloc 钩子。
 * - __rt_ffs：调度器优先级表等用到的「最低置 1 位」索引；无 CPU 指令版本时用
 *   查表实现（含 RT_USING_TINY_FFS 省 ROM 表）。
 */

#include <rtthread.h>

/* include rt_hw_backtrace macro defined in cpuport.h */
#define RT_HW_INCLUDE_CPUPORT
#include <rthw.h>

#define DBG_TAG           "kernel.service"
#ifdef RT_DEBUG_DEVICE
#define DBG_LVL           DBG_LOG
#else
#define DBG_LVL           DBG_WARNING
#endif /* defined (RT_DEBUG_DEVICE) */
#include <rtdbg.h>

#ifdef RT_USING_MODULE
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

#ifdef RT_USING_SMART
#include <lwp.h>
#include <lwp_user_mm.h>
#endif

/**
 * @addtogroup group_KernelService
 * @{
 */

/* 以下为板级弱符号、控制台、回溯、堆、FFS、断言等分段实现（中文分段标题见各节） */

#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
static rt_device_t _console_device = RT_NULL;
#endif

/* ========== BSP / arch 弱符号（无则默认行为：日志或空） ========== */

/* 微秒级忙等或定时器延时；无实现时仅告警，调用方勿假定已延时 */
rt_weak void rt_hw_us_delay(rt_uint32_t us)
{
    (void) us;
    LOG_W("rt_hw_us_delay() doesn't support for this board."
        "Please consider implementing rt_hw_us_delay() in another file.");
}

/* 硬复位入口；BSP 通常写看门狗或 AIRCR 等 */
rt_weak void rt_hw_cpu_reset(void)
{
    LOG_W("rt_hw_cpu_reset() doesn't support for this board."
        "Please consider implementing rt_hw_cpu_reset() in another file.");
    return;
}

/* 关机路径：默认关中断后 RT_ASSERT(0) 停住，避免误继续执行 */
rt_weak void rt_hw_cpu_shutdown(void)
{
    LOG_I("CPU shutdown...");
    LOG_W("Using default rt_hw_cpu_shutdown()."
        "Please consider implementing rt_hw_cpu_shutdown() in another file.");
    rt_hw_interrupt_disable();
    RT_ASSERT(0);
    return;
}

/**
 * @note can be overridden by cpuport.h which is defined by a specific arch
 * @note 取「当前调用点」的 fp/pc 作为回溯起点；GCC 用 __builtin_frame_address
 *       与 GNU 取址标签技巧，其它编译器则置 0 表示需 arch 自行实现。
 */
#ifndef RT_HW_BACKTRACE_FRAME_GET_SELF

#ifdef __GNUC__
    #define RT_HW_BACKTRACE_FRAME_GET_SELF(frame) do {          \
        (frame)->fp = (rt_uintptr_t)__builtin_frame_address(0U);   \
        (frame)->pc = ({__label__ pc; pc: (rt_uintptr_t)&&pc;});   \
    } while (0)

#else
    #define RT_HW_BACKTRACE_FRAME_GET_SELF(frame) do {  \
        (frame)->fp = 0;                                \
        (frame)->pc = 0;                                \
    } while (0)

#endif /* __GNUC__ */

#endif /* RT_HW_BACKTRACE_FRAME_GET_SELF */

/**
 * @brief Get the inner most frame of target thread
 *
 * @param thread the thread which frame belongs to
 * @param frame the specified frame to be unwound
 * @return rt_err_t 0 is succeed, otherwise a failure
 * @note 调试其它线程栈时由 arch 从 TCB/保存上下文解析最内层栈帧；未实现则 -RT_ENOSYS。
 */
rt_weak rt_err_t rt_hw_backtrace_frame_get(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    RT_UNUSED(thread);
    RT_UNUSED(frame);

    LOG_W("%s is not implemented", __func__);
    return -RT_ENOSYS;
}

/**
 * @brief Unwind the target frame
 *
 * @param thread the thread which frame belongs to
 * @param frame the specified frame to be unwound
 * @return rt_err_t 0 is succeed, otherwise a failure
 * @note 将 frame 推进到上一层调用者；成功返回 0，已到栈顶等非 0 表示停止展开。
 */
rt_weak rt_err_t rt_hw_backtrace_frame_unwind(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    RT_UNUSED(thread);
    RT_UNUSED(frame);

    LOG_W("%s is not implemented", __func__);
    return -RT_ENOSYS;
}

rt_weak const char *rt_hw_cpu_arch(void)
{
    return "unknown";
}

/**
 * @brief This function will show the version of rt-thread rtos
 * @note 启动时 ASCII Logo + 版本号 + 编译日期时间；Smart/Nano 变体字符串不同。
 */
void rt_show_version(void)
{
    rt_kprintf("\n \\ | /\n");
#if defined(RT_USING_SMART)
    rt_kprintf("- RT -     Thread Smart Operating System\n");
#elif defined(RT_USING_NANO)
    rt_kprintf("- RT -     Thread Nano Operating System\n");
#else
    rt_kprintf("- RT -     Thread Operating System\n");
#endif
    rt_kprintf(" / | \\     %d.%d.%d build %s %s\n",
               (rt_int32_t)RT_VERSION_MAJOR, (rt_int32_t)RT_VERSION_MINOR, (rt_int32_t)RT_VERSION_PATCH, __DATE__, __TIME__);
    rt_kprintf(" 2006 - 2024 Copyright by RT-Thread team\n");
}
RTM_EXPORT(rt_show_version);

#ifdef RT_USING_CONSOLE
#ifdef RT_USING_DEVICE
/**
 * @brief  This function returns the device using in console.
 *
 * @return Returns the console device pointer or RT_NULL.
 * @note 未 set 过则为 RT_NULL，此时 rt_kprintf 走 rt_hw_console_output。
 */
rt_device_t rt_console_get_device(void)
{
    return _console_device;
}
RTM_EXPORT(rt_console_get_device);

/**
 * @brief  This function will set a device as console device.
 * After set a device to console, all output of rt_kprintf will be
 * redirected to this new device.
 *
 * @param  name is the name of new console device.
 *
 * @return the old console device handler on successful, or RT_NULL on failure.
 * @note 新设备以 STREAM+RDWR 打开；旧设备若存在则 close。同名或未找到则保持原设备。
 */
rt_device_t rt_console_set_device(const char *name)
{
    rt_device_t old_device = _console_device;
    rt_device_t new_device = rt_device_find(name);

    if (new_device != RT_NULL && new_device != old_device)
    {
        if (old_device != RT_NULL)
        {
            /* close old console device */
            rt_device_close(old_device);
        }

        /* set new console device */
        rt_device_open(new_device, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_STREAM);
        _console_device = new_device;
    }

    return old_device;
}
RTM_EXPORT(rt_console_set_device);
#endif /* RT_USING_DEVICE */

rt_weak void rt_hw_console_output(const char *str)
{
    /* empty console output */
    RT_UNUSED(str);
}
RTM_EXPORT(rt_hw_console_output);

/* 无 RT_USING_DEVICE 或未 set 控制台设备时，逐字符/块输出由 BSP 实现（UART 轮询等） */

#ifdef RT_USING_THREADSAFE_PRINTF
/*
 * 线程安全打印：_syscon_lock 保护「当前占用控制台的线程」_pr_curr_user；
 * 占用时在无调度抢占临界区内写设备，避免与另一线程交叉；_prbuf_lock 保护
 * 静态 rt_log_buf，避免两个线程同时 vsnprintf 踩同一缓冲区。
 */

/* system console lock */
static struct rt_spinlock _syscon_lock = RT_SPINLOCK_INIT;
/* lock of kprintf buffer */
static struct rt_spinlock _prbuf_lock = RT_SPINLOCK_INIT;
/* current user of system console */
static rt_thread_t _pr_curr_user;

#ifdef RT_USING_DEBUG
static rt_base_t _pr_critical_level;
#endif /* RT_USING_DEBUG */

/* nested level of current user */
static volatile int _pr_curr_user_nested;

rt_thread_t rt_console_current_user(void)
{
    return _pr_curr_user;
}

static void _console_take(void)
{
    rt_ubase_t level = rt_spin_lock_irqsave(&_syscon_lock);
    rt_thread_t self_thread = rt_thread_self();
    rt_base_t critical_level;
    RT_UNUSED(critical_level);

    while (_pr_curr_user != self_thread)
    {
        if (_pr_curr_user == RT_NULL)
        {
            /* no preemption is allowed to avoid dead lock */
            critical_level = rt_enter_critical();
#ifdef RT_USING_DEBUG
            _pr_critical_level = _syscon_lock.critical_level;
            _syscon_lock.critical_level = critical_level;
#endif
            _pr_curr_user = self_thread;
            break;
        }
        else
        {
            /* 控制台已被其它线程占用：让出 CPU 后再抢锁轮询（非阻塞睡眠式自旋） */
            rt_spin_unlock_irqrestore(&_syscon_lock, level);
            rt_thread_yield();
            level = rt_spin_lock_irqsave(&_syscon_lock);
        }
    }

    _pr_curr_user_nested++;

    rt_spin_unlock_irqrestore(&_syscon_lock, level);
}

static void _console_release(void)
{
    rt_ubase_t level = rt_spin_lock_irqsave(&_syscon_lock);
    rt_thread_t self_thread = rt_thread_self();
    RT_UNUSED(self_thread);

    RT_ASSERT(_pr_curr_user == self_thread);

    _pr_curr_user_nested--;
    if (!_pr_curr_user_nested)
    {
        _pr_curr_user = RT_NULL;

#ifdef RT_USING_DEBUG
        rt_exit_critical_safe(_syscon_lock.critical_level);
        _syscon_lock.critical_level = _pr_critical_level;
#else
        rt_exit_critical();
#endif
    }
    rt_spin_unlock_irqrestore(&_syscon_lock, level);
}

#define CONSOLE_TAKE          _console_take()
#define CONSOLE_RELEASE       _console_release()
#define PRINTF_BUFFER_TAKE    rt_ubase_t level = rt_spin_lock_irqsave(&_prbuf_lock)
#define PRINTF_BUFFER_RELEASE rt_spin_unlock_irqrestore(&_prbuf_lock, level)
#else

#define CONSOLE_TAKE
#define CONSOLE_RELEASE
#define PRINTF_BUFFER_TAKE
#define PRINTF_BUFFER_RELEASE
#endif /* RT_USING_THREADSAFE_PRINTF */

/**
 * @brief This function will put string to the console.
 *
 * @param str is the string output to the console.
 * @note CONSOLE_TAKE/RELEASE 包裹整段输出；有设备则 write，否则 rt_hw_console_output。
 */
static void _kputs(const char *str, long len)
{
#ifdef RT_USING_DEVICE
    rt_device_t console_device = rt_console_get_device();
#endif /* RT_USING_DEVICE */

    CONSOLE_TAKE;

#ifdef RT_USING_DEVICE
    if (console_device == RT_NULL)
    {
        rt_hw_console_output(str);
    }
    else
    {
        rt_device_write(console_device, 0, str, len);
    }
#else
    RT_UNUSED(len);
    rt_hw_console_output(str);
#endif /* RT_USING_DEVICE */

    CONSOLE_RELEASE;
}

/**
 * @brief This function will put string to the console.
 *
 * @param str is the string output to the console.
 */
void rt_kputs(const char *str)
{
    if (!str)
    {
        return;
    }

    _kputs(str, rt_strlen(str));
}

/**
 * @brief This function will print a formatted string on system console.
 *
 * @param fmt is the format parameters.
 *
 * @return The number of characters actually written to buffer.
 * @note 静态 rt_log_buf 上 rt_vsnprintf，超长截断到 RT_CONSOLEBUF_SIZE-1；
 *       与 THREADSAFE_PRINTF 组合时缓冲区由 _prbuf_lock 保护。
 */
rt_weak int rt_kprintf(const char *fmt, ...)
{
    va_list args;
    rt_size_t length = 0;
    static char rt_log_buf[RT_CONSOLEBUF_SIZE];

    va_start(args, fmt);
    PRINTF_BUFFER_TAKE;

    /* the return value of vsnprintf is the number of bytes that would be
     * written to buffer had if the size of the buffer been sufficiently
     * large excluding the terminating null byte. If the output string
     * would be larger than the rt_log_buf, we have to adjust the output
     * length. */
    length = rt_vsnprintf(rt_log_buf, sizeof(rt_log_buf) - 1, fmt, args);
    if (length > RT_CONSOLEBUF_SIZE - 1)
    {
        length = RT_CONSOLEBUF_SIZE - 1;
    }

    _kputs(rt_log_buf, length);

    PRINTF_BUFFER_RELEASE;
    va_end(args);

    return length;
}
RTM_EXPORT(rt_kprintf);
#endif /* RT_USING_CONSOLE */

/* ========== 栈回溯（弱实现 + arch 帧操作） ========== */

/**
 * @brief Print backtrace of current thread to system console device
 *
 * @return rt_err_t 0 is success, otherwise a failure
 * @note 先取本线程当前帧，丢掉当前栈顶「噪声」帧再逐层 unwind 打印 PC。
 */
rt_weak rt_err_t rt_backtrace(void)
{
    struct rt_hw_backtrace_frame frame;
    rt_thread_t thread = rt_thread_self();

    RT_HW_BACKTRACE_FRAME_GET_SELF(&frame);
    if (!frame.fp)
        return -RT_EINVAL;

    /* we don't want this frame to be printed which is nearly garbage info */
    rt_hw_backtrace_frame_unwind(thread, &frame);

    return rt_backtrace_frame(thread, &frame);
}

/**
 * @brief Print backtrace from frame to system console device
 *
 * @param thread the thread which frame belongs to
 * @param frame where backtrace starts from
 * @return rt_err_t 0 is success, otherwise a failure
 * @note 默认实现只打印十六进制 PC 列表；符号解析需主机上 addr2line。
 */
rt_weak rt_err_t rt_backtrace_frame(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    long nesting = 0;

    rt_kprintf("please use: addr2line -e rtthread.elf -a -f\n");

    while (nesting < RT_BACKTRACE_LEVEL_MAX_NR)
    {
        rt_kprintf(" 0x%lx", (rt_ubase_t)frame->pc);
        if (rt_hw_backtrace_frame_unwind(thread, frame))
        {
            break;
        }
        nesting++;
    }
    rt_kprintf("\n");
    return RT_EOK;
}

/**
 * @brief Print backtrace from buffer to system console
 *
 * @param buffer where traced frames saved
 * @param buflen number of items in buffer
 * @return rt_err_t 0 is success, otherwise a failure
 * @note 将 rt_backtrace_to_buffer 等收集的 PC 数组格式化输出。
 */
rt_weak rt_err_t rt_backtrace_formatted_print(rt_ubase_t *buffer, long buflen)
{
    rt_kprintf("please use: addr2line -e rtthread.elf -a -f\n");

    for (rt_size_t i = 0; i < buflen && buffer[i] != 0; i++)
    {
        rt_kprintf(" 0x%lx", (rt_ubase_t)buffer[i]);
    }

    rt_kprintf("\n");
    return RT_EOK;
}


/**
 * @brief Print backtrace from frame to the given buffer
 *
 * @param thread the thread which frame belongs to
 * @param frame where backtrace starts from. NULL if it's the current one
 * @param skip the number of frames to discarded counted from calling function.
 *             Noted that the inner most frame is always discarded and not counted,
 *             which is obviously reasonable since that's this function itself.
 * @param buffer where traced frames saved
 * @param buflen max number of items can be saved in buffer. If there are no more
 *               than buflen items to be saved, there will be a NULL after the
 *               last saved item in the buffer.
 * @return rt_err_t 0 is success, otherwise a failure
 * @note skip 丢弃若干外层帧；最内层（本函数自身）在循环前已通过 unwind 丢掉。
 */
rt_weak rt_err_t rt_backtrace_to_buffer(rt_thread_t thread,
                                        struct rt_hw_backtrace_frame *frame,
                                        long skip,
                                        rt_ubase_t *buffer,
                                        long buflen)
{
    long nesting = 0;
    struct rt_hw_backtrace_frame cur_frame;

    if (!thread)
        return -RT_EINVAL;

    RT_ASSERT(rt_object_get_type(&thread->parent) == RT_Object_Class_Thread);

    if (!frame)
    {
        frame = &cur_frame;
        RT_HW_BACKTRACE_FRAME_GET_SELF(frame);
        if (!frame->fp)
            return -RT_EINVAL;
    }

    /* discard frames as required. The inner most is always threw. */
    do {
        rt_hw_backtrace_frame_unwind(thread, frame);
    } while (skip-- > 0);

    while (nesting < buflen)
    {
        *buffer++ = (rt_ubase_t)frame->pc;
        if (rt_hw_backtrace_frame_unwind(thread, frame))
        {
            break;
        }
        nesting++;
    }

    if (nesting < buflen)
        *buffer = RT_NULL;

    return RT_EOK;
}

/**
 * @brief Print backtrace of a thread to system console device
 *
 * @param thread which call stack is traced
 * @return rt_err_t 0 is success, otherwise a failure
 * @note 非弱函数；依赖 rt_hw_backtrace_frame_get 能解析目标线程栈顶。
 */
rt_err_t rt_backtrace_thread(rt_thread_t thread)
{
    rt_err_t rc;
    struct rt_hw_backtrace_frame frame;
    if (thread)
    {
        rc = rt_hw_backtrace_frame_get(thread, &frame);
        if (rc == RT_EOK)
        {
            rc = rt_backtrace_frame(thread, &frame);
        }
    }
    else
    {
        rc = -RT_EINVAL;
    }
    return rc;
}

#if defined(RT_USING_LIBC) && defined(RT_USING_FINSH)
#include <stdlib.h> /* for string service */

static void cmd_backtrace(int argc, char** argv)
{
    rt_uintptr_t pid;
    char *end_ptr;

    if (argc != 2)
    {
        if (argc == 1)
        {
            rt_kprintf("[INFO] No thread specified\n"
                "[HELP] You can use commands like: backtrace %p\n"
                "Printing backtrace of calling stack...\n",
                rt_thread_self());
            rt_backtrace();
            return ;
        }
        else
        {
            rt_kprintf("please use: backtrace [thread_address]\n");
            return;
        }
    }

    pid = strtoul(argv[1], &end_ptr, 0);
    if (end_ptr == argv[1])
    {
        rt_kprintf("Invalid input: %s\n", argv[1]);
        return ;
    }

    if (pid && rt_object_get_type((void *)pid) == RT_Object_Class_Thread)
    {
        rt_thread_t target = (rt_thread_t)pid;
        rt_kprintf("backtrace %s(0x%lx), from %s\n", target->parent.name, pid, argv[1]);
        rt_backtrace_thread(target);
    }
    else
        rt_kprintf("Invalid pid: %ld\n", pid);
}
MSH_CMD_EXPORT_ALIAS(cmd_backtrace, backtrace, print backtrace of a thread);

#endif /* RT_USING_LIBC */

#if defined(RT_USING_HEAP) && !defined(RT_USING_USERHEAP)
/*
 * ========== 内核系统堆（与 memheap/slab/small 三选一绑定） ==========
 * 锁策略：RT_USING_HEAP_ISR 用自旋锁（可在 ISR 路径配合策略使用）；
 * 否则若 RT_USING_MUTEX 用互斥量；再否则 rt_enter_critical 关抢占。
 */

#ifdef RT_USING_HOOK
static void (*rt_malloc_hook)(void **ptr, rt_size_t size);
static void (*rt_realloc_entry_hook)(void **ptr, rt_size_t size);
static void (*rt_realloc_exit_hook)(void **ptr, rt_size_t size);
static void (*rt_free_hook)(void **ptr);

/**
 * @ingroup group_Hook
 * @{
 */

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is allocated from heap memory.
 *
 * @param hook the hook function.
 * @note 在 _heap_unlock 之后调用，ptr 为实际返回指针，可用于统计或检测泄漏。
 */
void rt_malloc_sethook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_malloc_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is allocated from heap memory.
 *
 * @param hook the hook function.
 * @note realloc 加锁前调用（entry），可配合 exit 钩子做 sanitizer 等。
 */
void rt_realloc_set_entry_hook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_realloc_entry_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is allocated from heap memory.
 *
 * @param hook the hook function.
 * @note realloc 解锁后调用（exit），ptr 指向新块地址。
 */
void rt_realloc_set_exit_hook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_realloc_exit_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is released to heap memory.
 *
 * @param hook the hook function
 * @note 在加锁释放内存之前调用，可改写 *ptr（例如置毒指针）需自行把握语义。
 */
void rt_free_sethook(void (*hook)(void **ptr))
{
    rt_free_hook = hook;
}

/**@}*/

#endif /* RT_USING_HOOK */

#if defined(RT_USING_HEAP_ISR)
static struct rt_spinlock _heap_spinlock;
#elif defined(RT_USING_MUTEX)
static struct rt_mutex _lock;
#endif

rt_inline void _heap_lock_init(void)
{
#if defined(RT_USING_HEAP_ISR)
    rt_spin_lock_init(&_heap_spinlock);
#elif defined(RT_USING_MUTEX)
    rt_mutex_init(&_lock, "heap", RT_IPC_FLAG_PRIO);
#endif
}

rt_inline rt_base_t _heap_lock(void)
{
#if defined(RT_USING_HEAP_ISR)
    return rt_spin_lock_irqsave(&_heap_spinlock);
#elif defined(RT_USING_MUTEX)
    /* 调度器未就绪或 idle 前 rt_thread_self() 可能为 NULL，此时不拿互斥量 */
    if (rt_thread_self())
        return rt_mutex_take(&_lock, RT_WAITING_FOREVER);
    else
        return RT_EOK;
#else
    rt_enter_critical();
    return RT_EOK;
#endif
}

rt_inline void _heap_unlock(rt_base_t level)
{
#if defined(RT_USING_HEAP_ISR)
    rt_spin_unlock_irqrestore(&_heap_spinlock, level);
#elif defined(RT_USING_MUTEX)
    RT_ASSERT(level == RT_EOK);
    if (rt_thread_self())
        rt_mutex_release(&_lock);
#else
    rt_exit_critical();
#endif
}

#ifdef RT_USING_UTESTCASES
/* export to utest to observe the inner statements */
#ifdef _MSC_VER
#define rt_heap_lock() _heap_lock()
#define rt_heap_unlock() _heap_unlock()
#else
rt_base_t rt_heap_lock(void) __attribute__((alias("_heap_lock")));
void rt_heap_unlock(rt_base_t level) __attribute__((alias("_heap_unlock")));
#endif /* _MSC_VER */
#endif

#if defined(RT_USING_SMALL_MEM_AS_HEAP)
static rt_smem_t system_heap;
rt_inline void _smem_info(rt_size_t *total,
    rt_size_t *used, rt_size_t *max_used)
{
    if (total)
        *total = system_heap->total;
    if (used)
        *used = system_heap->used;
    if (max_used)
        *max_used = system_heap->max;
}
#define _MEM_INIT(_name, _start, _size) \
    system_heap = rt_smem_init(_name, _start, _size)
#define _MEM_MALLOC(_size)  \
    rt_smem_alloc(system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)\
    rt_smem_realloc(system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr) \
    rt_smem_free(_ptr)
#define _MEM_INFO(_total, _used, _max)  \
    _smem_info(_total, _used, _max)
#elif defined(RT_USING_MEMHEAP_AS_HEAP)
static struct rt_memheap system_heap;
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
void _memheap_free(void *rmem);
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize);
#define _MEM_INIT(_name, _start, _size) \
    do {\
        rt_memheap_init(&system_heap, _name, _start, _size); \
        system_heap.locked = RT_TRUE; \
    } while(0)
#define _MEM_MALLOC(_size)  \
    _memheap_alloc(&system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)    \
    _memheap_realloc(&system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr)   \
    _memheap_free(_ptr)
#define _MEM_INFO(_total, _used, _max)   \
    rt_memheap_info(&system_heap, _total, _used, _max)
#elif defined(RT_USING_SLAB_AS_HEAP)
static rt_slab_t system_heap;
rt_inline void _slab_info(rt_size_t *total,
    rt_size_t *used, rt_size_t *max_used)
{
    if (total)
        *total = system_heap->total;
    if (used)
        *used = system_heap->used;
    if (max_used)
        *max_used = system_heap->max;
}
#define _MEM_INIT(_name, _start, _size) \
    system_heap = rt_slab_init(_name, _start, _size)
#define _MEM_MALLOC(_size)  \
    rt_slab_alloc(system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)    \
    rt_slab_realloc(system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr) \
    rt_slab_free(system_heap, _ptr)
#define _MEM_INFO       _slab_info
#else
#define _MEM_INIT(...)
#define _MEM_MALLOC(...)     RT_NULL
#define _MEM_REALLOC(...)    RT_NULL
#define _MEM_FREE(...)
#define _MEM_INFO(...)
#endif

/**
 * @brief This function will do the generic system heap initialization.
 *
 * @param begin_addr the beginning address of system page.
 *
 * @param end_addr the end address of system page.
 * @note 起止地址按 RT_ALIGN_SIZE 对齐后再 _MEM_INIT；随后 _heap_lock_init。
 */
void rt_system_heap_init_generic(void *begin_addr, void *end_addr)
{
    rt_uintptr_t begin_align = RT_ALIGN((rt_uintptr_t)begin_addr, RT_ALIGN_SIZE);
    rt_uintptr_t end_align   = RT_ALIGN_DOWN((rt_uintptr_t)end_addr, RT_ALIGN_SIZE);

    RT_ASSERT(end_align > begin_align);

    /* Initialize system memory heap */
    _MEM_INIT("heap", (void *)begin_align, end_align - begin_align);
    /* Initialize multi thread contention lock */
    _heap_lock_init();
}

/**
 * @brief This function will init system heap. User can override this API to
 *        complete other works, like heap sanitizer initialization.
 *
 * @param begin_addr the beginning address of system page.
 *
 * @param end_addr the end address of system page.
 * @note 弱符号默认转 rt_system_heap_init_generic；可 override 做 poison/统计初始化。
 */
rt_weak void rt_system_heap_init(void *begin_addr, void *end_addr)
{
    rt_system_heap_init_generic(begin_addr, end_addr);
}

/**
 * @brief Allocate a block of memory with a minimum of 'size' bytes.
 *
 * @param size is the minimum size of the requested block in bytes.
 *
 * @return the pointer to allocated memory or NULL if no free memory was found.
 * @note 堆操作在 _heap_lock 临界区内完成，再调 malloc 钩子（若配置）。
 */
rt_weak void *rt_malloc(rt_size_t size)
{
    rt_base_t level;
    void *ptr;

    /* Enter critical zone */
    level = _heap_lock();
    /* allocate memory block from system heap */
    ptr = _MEM_MALLOC(size);
    /* Exit critical zone */
    _heap_unlock(level);
    /* call 'rt_malloc' hook */
    RT_OBJECT_HOOK_CALL(rt_malloc_hook, (&ptr, size));
    return ptr;
}
RTM_EXPORT(rt_malloc);

/**
 * @brief This function will change the size of previously allocated memory block.
 *
 * @param ptr is the pointer to memory allocated by rt_malloc.
 *
 * @param newsize is the required new size.
 *
 * @return the changed memory block address.
 * @note entry/exit 钩子包在加锁 realloc 两侧，便于跟踪指针迁移。
 */
rt_weak void *rt_realloc(void *ptr, rt_size_t newsize)
{
    rt_base_t level;
    void *nptr;

    /* Entry hook */
    RT_OBJECT_HOOK_CALL(rt_realloc_entry_hook, (&ptr, newsize));
    /* Enter critical zone */
    level = _heap_lock();
    /* Change the size of previously allocated memory block */
    nptr = _MEM_REALLOC(ptr, newsize);
    /* Exit critical zone */
    _heap_unlock(level);
    /* Exit hook */
    RT_OBJECT_HOOK_CALL(rt_realloc_exit_hook, (&nptr, newsize));
    return nptr;
}
RTM_EXPORT(rt_realloc);

/**
 * @brief  This function will contiguously allocate enough space for count objects
 *         that are size bytes of memory each and returns a pointer to the allocated
 *         memory.
 *
 * @note   The allocated memory is filled with bytes of value zero.
 *
 * @param  count is the number of objects to allocate.
 *
 * @param  size is the size of one object to allocate.
 *
 * @return pointer to allocated memory / NULL pointer if there is an error.
 * @note count*size 可能溢出，调用方应保证乘积合法；成功则 rt_memset 清零。
 */
rt_weak void *rt_calloc(rt_size_t count, rt_size_t size)
{
    void *p;

    /* allocate 'count' objects of size 'size' */
    p = rt_malloc(count * size);
    /* zero the memory */
    if (p)
    {
        rt_memset(p, 0, count * size);
    }
    return p;
}
RTM_EXPORT(rt_calloc);

/**
 * @brief This function will release the previously allocated memory block by
 *        rt_malloc. The released memory block is taken back to system heap.
 *
 * @param ptr the address of memory which will be released.
 * @note 先 free 钩子再判空加锁释放；ptr==NULL 为合法空操作。
 */
rt_weak void rt_free(void *ptr)
{
    rt_base_t level;

    /* call 'rt_free' hook */
    RT_OBJECT_HOOK_CALL(rt_free_hook, (&ptr));
    /* NULL check */
    if (ptr == RT_NULL) return;
    /* Enter critical zone */
    level = _heap_lock();
    _MEM_FREE(ptr);
    /* Exit critical zone */
    _heap_unlock(level);
}
RTM_EXPORT(rt_free);

/**
* @brief This function will caculate the total memory, the used memory, and
*        the max used memory.
*
* @param total is a pointer to get the total size of the memory.
*
* @param used is a pointer to get the size of memory used.
*
* @param max_used is a pointer to get the maximum memory used.
* @note 各指针可为 NULL 表示不关心该项；在堆锁内读后端堆统计。
*/
rt_weak void rt_memory_info(rt_size_t *total,
                            rt_size_t *used,
                            rt_size_t *max_used)
{
    rt_base_t level;

    /* Enter critical zone */
    level = _heap_lock();
    _MEM_INFO(total, used, max_used);
    /* Exit critical zone */
    _heap_unlock(level);
}
RTM_EXPORT(rt_memory_info);

#if defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP)
void *rt_page_alloc(rt_size_t npages)
{
    rt_base_t level;
    void *ptr;

    /* Enter critical zone */
    level = _heap_lock();
    /* alloc page */
    ptr = rt_slab_page_alloc(system_heap, npages);
    /* Exit critical zone */
    _heap_unlock(level);
    return ptr;
}

void rt_page_free(void *addr, rt_size_t npages)
{
    rt_base_t level;

    /* Enter critical zone */
    level = _heap_lock();
    /* free page */
    rt_slab_page_free(system_heap, addr, npages);
    /* Exit critical zone */
    _heap_unlock(level);
}
#endif

/**
 * @brief  This function allocates a memory block, which address is aligned to the
 * specified alignment size.
 *
 * @param  size is the allocated memory block size.
 *
 * @param  align is the alignment size.
 *
 * @return The memory block address was returned successfully, otherwise it was
 *         returned empty RT_NULL.
 * @note 多申请 align+对齐填充，在「对齐后地址」前一字存放真实 rt_malloc 指针，
 *       供 rt_free_align 取回；对齐宽度向上取到 void* 粒度。
 */
rt_weak void *rt_malloc_align(rt_size_t size, rt_size_t align)
{
    void *ptr = RT_NULL;
    void *align_ptr = RT_NULL;
    int uintptr_size = 0;
    rt_size_t align_size = 0;

    /* sizeof pointer */
    uintptr_size = sizeof(void*);
    uintptr_size -= 1;

    /* align the alignment size to uintptr size byte */
    align = ((align + uintptr_size) & ~uintptr_size);

    /* get total aligned size */
    align_size = ((size + uintptr_size) & ~uintptr_size) + align;
    /* allocate memory block from heap */
    ptr = rt_malloc(align_size);
    if (ptr != RT_NULL)
    {
        /* the allocated memory block is aligned */
        if (((rt_uintptr_t)ptr & (align - 1)) == 0)
        {
            align_ptr = (void *)((rt_uintptr_t)ptr + align);
        }
        else
        {
            align_ptr = (void *)(((rt_uintptr_t)ptr + (align - 1)) & ~(align - 1));
        }

        /* set the pointer before alignment pointer to the real pointer */
        *((rt_uintptr_t *)((rt_uintptr_t)align_ptr - sizeof(void *))) = (rt_uintptr_t)ptr;

        ptr = align_ptr;
    }

    return ptr;
}
RTM_EXPORT(rt_malloc_align);

/**
 * @brief This function release the memory block, which is allocated by
 * rt_malloc_align function and address is aligned.
 *
 * @param ptr is the memory block pointer.
 * @note 必须用 rt_malloc_align 返回的用户指针释放；内部读对齐首址前 sizeof(void*) 的 cookie。
 */
rt_weak void rt_free_align(void *ptr)
{
    void *real_ptr = RT_NULL;

    /* NULL check */
    if (ptr == RT_NULL) return;
    real_ptr = (void *) * (rt_uintptr_t *)((rt_uintptr_t)ptr - sizeof(void *));
    rt_free(real_ptr);
}
RTM_EXPORT(rt_free_align);
#endif /* RT_USING_HEAP */

/* ========== __rt_ffs：最低置 1 位的序号（1-based），value==0 返回 0 ========== */

#ifndef RT_USING_CPU_FFS
#ifdef RT_USING_TINY_FFS
const rt_uint8_t __lowest_bit_bitmap[] =
{
    /*  0 - 7  */  0,  1,  2, 27,  3, 24, 28, 32,
    /*  8 - 15 */  4, 17, 25, 31, 29, 12, 32, 14,
    /* 16 - 23 */  5,  8, 18, 32, 26, 23, 32, 16,
    /* 24 - 31 */ 30, 11, 13,  7, 32, 22, 15, 10,
    /* 32 - 36 */  6, 21,  9, 20, 19
};

/**
 * @brief This function finds the first bit set (beginning with the least significant bit)
 * in value and return the index of that bit.
 *
 * Bits are numbered starting at 1 (the least significant bit).  A return value of
 * zero from any of these functions means that the argument was zero.
 *
 * @param value is the value to find the first bit set in.
 *
 * @return return the index of the first bit set. If value is 0, then this function
 * shall return 0.
 * @note TINY 表仅 37 字节；用 (v&(v-1))^v 将最低 1 位孤立后查表得位序。
 */
int __rt_ffs(int value)
{
    return __lowest_bit_bitmap[(rt_uint32_t)(value & (value - 1) ^ value) % 37];
}
#else
const rt_uint8_t __lowest_bit_bitmap[] =
{
    /* 00 */ 0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 10 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 20 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 30 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 40 */ 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 50 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 60 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 70 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 80 */ 7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 90 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* A0 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* B0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* C0 */ 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* D0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* E0 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* F0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
};

/**
 * @brief This function finds the first bit set (beginning with the least significant bit)
 * in value and return the index of that bit.
 *
 * Bits are numbered starting at 1 (the least significant bit).  A return value of
 * zero from any of these functions means that the argument was zero.
 *
 * @param value is the value to find the first bit set in.
 *
 * @return Return the index of the first bit set. If value is 0, then this function
 *         shall return 0.
 * @note 按字节分段查 256 项表，比 TINY 版费 ROM、分支可预测性更好。
 */
int __rt_ffs(int value)
{
    if (value == 0)
    {
        return 0;
    }

    if (value & 0xff)
    {
        return __lowest_bit_bitmap[value & 0xff] + 1;
    }

    if (value & 0xff00)
    {
        return __lowest_bit_bitmap[(value & 0xff00) >> 8] + 9;
    }

    if (value & 0xff0000)
    {
        return __lowest_bit_bitmap[(value & 0xff0000) >> 16] + 17;
    }

    return __lowest_bit_bitmap[(value & 0xff000000) >> 24] + 25;
}
#endif /* RT_USING_TINY_FFS */
#endif /* RT_USING_CPU_FFS */

#ifdef RT_DEBUGING_ASSERT
/* RT_ASSERT(EX)'s hook */

void (*rt_assert_hook)(const char *ex, const char *func, rt_size_t line);

/**
 * This function will set a hook function to RT_ASSERT(EX). It will run when the expression is false.
 *
 * @param hook is the hook function.
 * @note 未设置钩子时默认 kprintf + 回溯 + 死循环停机；模块内断言可走 dlmodule_exit。
 */
void rt_assert_set_hook(void (*hook)(const char *ex, const char *func, rt_size_t line))
{
    rt_assert_hook = hook;
}

/**
 * The RT_ASSERT function.
 *
 * @param ex_string is the assertion condition string.
 *
 * @param func is the function name when assertion.
 *
 * @param line is the file line number when assertion.
 * @note 由 RT_ASSERT 宏展开调用；dummy 死循环便于接 JTAG 查现场。
 */
void rt_assert_handler(const char *ex_string, const char *func, rt_size_t line)
{
    volatile char dummy = 0;

    if (rt_assert_hook == RT_NULL)
    {
#ifdef RT_USING_MODULE
        if (dlmodule_self())
        {
            /* close assertion module */
            dlmodule_exit(-1);
        }
        else
#endif /*RT_USING_MODULE*/
        {
            rt_kprintf("(%s) assertion failed at function:%s, line number:%d \n", ex_string, func, line);
            rt_backtrace();
            while (dummy == 0);
        }
    }
    else
    {
        rt_assert_hook(ex_string, func, line);
    }
}
RTM_EXPORT(rt_assert_handler);
#endif /* RT_DEBUGING_ASSERT */

/**@}*/
