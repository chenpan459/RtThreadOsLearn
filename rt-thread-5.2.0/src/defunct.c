/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-08-30     heyuanjie87  the first version
 *
 */

/*
 * 本文件职责（中文概要）：
 * 「已退出但尚未销毁」的线程不立刻在自身上下文里释放栈/对象，而是进入 _rt_thread_defunct 延迟队列，
 * 由后台执行 rt_defunct_execute() 统一做 detach、cleanup、释放堆栈与 rt_object_delete 等收尾。
 * - 单核常见路径：在 idle 线程中调用 rt_defunct_execute()（见 idle.c）。
 * - SMP 或 SMART：额外创建低优先级内核线程 tsystem，通过信号量在入队时被唤醒，专门消费延迟队列，
 *   避免与 idle 争用且便于在多核场景下集中处理。
 */

#include <rthw.h>
#include <rtthread.h>

#ifndef SYSTEM_THREAD_STACK_SIZE
#define SYSTEM_THREAD_STACK_SIZE IDLE_THREAD_STACK_SIZE
#endif
/* 延迟销毁队列：链表头，节点为各线程的就绪链表节点 RT_THREAD_LIST_NODE */
static rt_list_t          _rt_thread_defunct = RT_LIST_OBJECT_INIT(_rt_thread_defunct);
/* 保护入队/出队与 idle、tsystem 并发访问 */
static struct rt_spinlock _defunct_spinlock;
#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
/* 专用于执行 rt_defunct_execute 的内核线程（阻塞在 system_sem 上，入队时 post 唤醒） */
static struct rt_thread rt_system_thread;
rt_align(RT_ALIGN_SIZE) static rt_uint8_t rt_system_stack[SYSTEM_THREAD_STACK_SIZE];
static struct rt_semaphore system_sem;
#endif

/**
 * @brief Enqueue a thread to defunct queue.
 *
 * @param thread the thread to be enqueued.
 *
 * @note It must be called between rt_hw_interrupt_disable and rt_hw_interrupt_enable
 * @note 中文补充：队列由 _defunct_spinlock 保护；SMP/SMART 下入队后 release 信号量唤醒 tsystem 线程执行回收。
 */
void rt_thread_defunct_enqueue(rt_thread_t thread)
{
    rt_base_t level;
    level = rt_spin_lock_irqsave(&_defunct_spinlock);
    rt_list_insert_after(&_rt_thread_defunct, &RT_THREAD_LIST_NODE(thread));
    rt_spin_unlock_irqrestore(&_defunct_spinlock, level);
#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
    /* 通知专用回收线程：队列中已有待处理项 */
    rt_sem_release(&system_sem);
#endif
}

/**
 * @brief Dequeue a thread from defunct queue.
 * @note 中文：队列为空时返回 RT_NULL；须与 enqueue 使用同一把自旋锁保证一致性。
 */
rt_thread_t rt_thread_defunct_dequeue(void)
{
    rt_base_t   level;
    rt_thread_t thread = RT_NULL;
    rt_list_t  *l      = &_rt_thread_defunct;

    level = rt_spin_lock_irqsave(&_defunct_spinlock);
    if (!rt_list_isempty(l))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(l->next);
        rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    }
    rt_spin_unlock_irqrestore(&_defunct_spinlock, level);

    return thread;
}

/**
 * @brief This function will perform system background job when system idle.
 * @note 中文：一次调用会尽量清空当前队列中所有待销毁线程，避免残留；可在 idle 或 tsystem 中调用。
 */
void rt_defunct_execute(void)
{
    /* Loop until there is no dead thread. So one call to rt_defunct_execute
     * will do all the cleanups. */
    /* 一次调用内循环出队直到空，减少多次进入空闲的开销 */
    while (1)
    {
        rt_thread_t thread;
        rt_bool_t   object_is_systemobject;
        void (*cleanup)(struct rt_thread *tid);

#ifdef RT_USING_MODULE
        struct rt_dlmodule *module = RT_NULL;
#endif
        /* get defunct thread */
        thread = rt_thread_defunct_dequeue();
        if (thread == RT_NULL)
        {
            break;
        }

#ifdef RT_USING_MODULE
        /* 动态模块线程：先销毁所属模块再释放线程资源 */
        module = (struct rt_dlmodule *)thread->parent.module_id;
        if (module)
        {
            dlmodule_destroy(module);
        }
#endif

#ifdef RT_USING_SIGNALS
        rt_thread_free_sig(thread);
#endif

        /* store the point of "thread->cleanup" avoid to lose */
        /* 先缓存 cleanup：后续 detach/delete 可能改写线程内存布局 */
        cleanup = thread->cleanup;

        /* if it's a system object, detach it */
        /* 静态线程：从对象容器摘除，但不走堆释放路径 */
        object_is_systemobject = rt_object_is_systemobject((rt_object_t)thread);
        if (object_is_systemobject == RT_TRUE)
        {
            /* detach this object */
            rt_object_detach((rt_object_t)thread);
        }

        /* invoke thread cleanup */
        /* 用户注册的收尾回调（如释放 thread 关联的私有数据） */
        if (cleanup != RT_NULL)
        {
            cleanup(thread);
        }

#ifdef RT_USING_HEAP
#ifdef RT_USING_MEM_PROTECTION
        if (thread->mem_regions != RT_NULL)
        {
            RT_KERNEL_FREE(thread->mem_regions);
        }
#endif
        /* if need free, delete it */
        /* 动态创建的线程：释放栈内存并删除线程内核对象 */
        if (object_is_systemobject == RT_FALSE)
        {
            /* release thread's stack */
#ifdef RT_USING_HW_STACK_GUARD
            RT_KERNEL_FREE(thread->stack_buf);
#else
            RT_KERNEL_FREE(thread->stack_addr);
#endif
            /* delete thread object */
            rt_object_delete((rt_object_t)thread);
        }
#endif
    }
}

#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
/* 阻塞等待信号量，被 enqueue 唤醒后批量执行 rt_defunct_execute */
static void rt_thread_system_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        int ret = rt_sem_take(&system_sem, RT_WAITING_FOREVER);
        if (ret != RT_EOK)
        {
            rt_kprintf("failed to sem_take() error %d\n", ret);
            RT_ASSERT(0);
        }
        rt_defunct_execute();
    }
}
#endif

/**
 * @brief 初始化延迟销毁子系统：自旋锁；在 SMP/SMART 下创建 tsystem 回收线程。
 * @note 中文：须在调度器启动前与其它内核线程初始化顺序一致调用（见 components.c 中 rtthread_startup）。
 */
void rt_thread_defunct_init(void)
{
    /* 回收线程优先级为 MAX-2，须保证优先级档位足够 */
    RT_ASSERT(RT_THREAD_PRIORITY_MAX > 2);

    rt_spin_lock_init(&_defunct_spinlock);

#if defined(RT_USING_SMP) || defined(RT_USING_SMART)
    /* 初值为 0：无待回收线程时 tsystem 阻塞；每次 enqueue 后 +1 唤醒 */
    rt_sem_init(&system_sem, "defunct", 0, RT_IPC_FLAG_FIFO);

    /* create defunct thread */
    /* 极低优先级专用线程，避免占用业务实时任务；栈尺寸同 IDLE 默认档 */
    rt_thread_init(&rt_system_thread,
                   "tsystem",
                   rt_thread_system_entry,
                   RT_NULL,
                   rt_system_stack,
                   sizeof(rt_system_stack),
                   RT_THREAD_PRIORITY_MAX - 2,
                   32);
    /* startup */
    rt_thread_startup(&rt_system_thread);
#endif
}
