/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-14     Bernard      the first version
 * 2006-04-25     Bernard      implement semaphore
 * 2006-05-03     Bernard      add RT_IPC_DEBUG
 *                             modify the type of IPC waiting time to rt_int32_t
 * 2006-05-10     Bernard      fix the semaphore take bug and add IPC object
 * 2006-05-12     Bernard      implement mailbox and message queue
 * 2006-05-20     Bernard      implement mutex
 * 2006-05-23     Bernard      implement fast event
 * 2006-05-24     Bernard      implement event
 * 2006-06-03     Bernard      fix the thread timer init bug
 * 2006-06-05     Bernard      fix the mutex release bug
 * 2006-06-07     Bernard      fix the message queue send bug
 * 2006-08-04     Bernard      add hook support
 * 2009-05-21     Yi.qiu       fix the sem release bug
 * 2009-07-18     Bernard      fix the event clear bug
 * 2009-09-09     Bernard      remove fast event and fix ipc release bug
 * 2009-10-10     Bernard      change semaphore and mutex value to unsigned value
 * 2009-10-25     Bernard      change the mb/mq receive timeout to 0 if the
 *                             re-calculated delta tick is a negative number.
 * 2009-12-16     Bernard      fix the rt_ipc_object_suspend issue when IPC flag
 *                             is RT_IPC_FLAG_PRIO
 * 2010-01-20     mbbill       remove rt_ipc_object_decrease function.
 * 2010-04-20     Bernard      move memcpy outside interrupt disable in mq
 * 2010-10-26     yi.qiu       add module support in rt_mp_delete and rt_mq_delete
 * 2010-11-10     Bernard      add IPC reset command implementation.
 * 2011-12-18     Bernard      add more parameter checking in message queue
 * 2013-09-14     Grissiom     add an option check in rt_event_recv
 * 2018-10-02     Bernard      add 64bit support for mailbox
 * 2019-09-16     tyx          add send wait support for message queue
 * 2020-07-29     Meco Man     fix thread->event_set/event_info when received an
 *                             event without pending
 * 2020-10-11     Meco Man     add value overflow-check code
 * 2021-01-03     Meco Man     implement rt_mb_urgent()
 * 2021-05-30     Meco Man     implement rt_mutex_trytake()
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to ipc.c
 * 2022-01-24     THEWON       let rt_mutex_take return thread->error when using signal
 * 2022-04-08     Stanley      Correct descriptions
 * 2022-10-15     Bernard      add nested mutex feature
 * 2022-10-16     Bernard      add prioceiling feature in mutex
 * 2023-04-16     Xin-zheqi    redesigen queue recv and send function return real message size
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 */

/*
 * 本文件职责（中文概要）
 *
 * 实现内核进程间通信（IPC）对象，各子模块由 Kconfig 宏包裹，未使能时不参与编译：
 * - 信号量（RT_USING_SEMAPHORE）：计数、二值、静态/动态创建与控制。
 * - 互斥量（RT_USING_MUTEX）：优先级继承/天花板、嵌套持有、与调度器协作。
 * - 事件（RT_USING_EVENT）：按位同步与唤醒。
 * - 邮箱（RT_USING_MAILBOX）：定长槽位传递 rt_ubase_t 消息。
 * - 消息队列（RT_USING_MESSAGEQUEUE）：变长消息 FIFO，可选优先级队列（RT_USING_MESSAGEQUEUE_PRIORITY）。
 *
 * 文件前部为各 IPC 共用的基础设施：struct rt_ipc_object 挂起链表、rt_ipc_list_suspend/resume、
 * rt_susp_list_dequeue / rt_susp_list_resume_all* 等，在持调度锁或自旋锁下把阻塞线程移入/移出挂起队列并触发调度。
 */

#include <rtthread.h>
#include <rthw.h>

#define DBG_TAG           "kernel.ipc"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#define GET_MESSAGEBYTE_ADDR(msg)               ((struct rt_mq_message *) msg + 1)
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
extern void (*rt_object_trytake_hook)(struct rt_object *object);
extern void (*rt_object_take_hook)(struct rt_object *object);
extern void (*rt_object_put_hook)(struct rt_object *object);
#endif /* RT_USING_HOOK */

/*===========================================================================*/
/* 通用 IPC 基础设施：挂起链表、挂起/唤醒辅助（不依赖具体 sem/mutex/mb/mq 类型） */
/*===========================================================================*/

/**
 * @addtogroup group_IPC
 * @{
 */

/**
 * @brief    This function will initialize an IPC object, such as semaphore, mutex, messagequeue and mailbox.
 *
 * @note     Executing this function will complete an initialization of the suspend thread list of the ipc object.
 * @note     仅初始化 ipc->suspend_thread 链表头；各类型对象另有自己的成员初始化。
 *
 * @param    ipc is a pointer to the IPC object.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           When the return value is any other values, it means the initialization failed.
 *
 * @warning  This function can be called from all IPC initialization and creation.
 */
rt_inline rt_err_t _ipc_object_init(struct rt_ipc_object *ipc)
{
    /* initialize ipc object */
    rt_list_init(&(ipc->suspend_thread));

    return RT_EOK;
}


/**
 * @brief   Dequeue a thread from suspended list and set it to ready. The 2 are
 *          taken as an atomic operation, so if a thread is returned, it's
 *          resumed by us, not any other threads or async events. This is useful
 *          if a consumer may be resumed by timeout, signals... besides its
 *          producer.
 *
 * @param   susp_list the list thread dequeued from. RT_NULL if no list.
 * @param   thread_error thread error number of the resuming thread.
 *          A negative value in this set will be discarded, and thread error
 *          will not be changed.
 *
 * @return  struct rt_thread * RT_NULL if failed, otherwise the thread resumed
 * @note     从 susp_list 摘头结点并 rt_sched_thread_ready；调用方须已 rt_sched_lock。
 */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error)
{
    rt_sched_lock_level_t slvl;
    rt_thread_t thread;
    rt_err_t error;

    RT_SCHED_DEBUG_IS_UNLOCKED;
    RT_ASSERT(susp_list != RT_NULL);

    rt_sched_lock(&slvl);
    if (!rt_list_isempty(susp_list))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(susp_list->next);
        error = rt_sched_thread_ready(thread);

        if (error)
        {
            LOG_D("%s [error:%d] failed to resume thread:%p from suspended list",
                  __func__, error, thread);

            thread = RT_NULL;
        }
        else
        {
            /* thread error should not be a negative value */
            if (thread_error >= 0)
            {
                /* set thread error code to notified resuming thread */
                thread->error = thread_error;
            }
        }
    }
    else
    {
        thread = RT_NULL;
    }
    rt_sched_unlock(slvl);

    LOG_D("resume thread:%s\n", thread->parent.name);

    return thread;
}


/**
 * @brief   This function will resume all suspended threads in the IPC object list,
 *          including the suspended list of IPC object, and private list of mailbox etc.
 *
 * @note    This function will resume all threads in the IPC object list.
 *          By contrast, the rt_ipc_list_resume() function will resume a suspended thread in the list of a IPC object.
 *
 * @param   susp_list is a pointer to a suspended thread list of the IPC object.
 * @param   thread_error thread error number of the resuming thread.
 *          A negative value in this set will be discarded, and thread error
 *          will not be changed.
 *
 * @return  Return the operation status. When the return value is RT_EOK, the function is successfully executed.
 *          When the return value is any other values, it means this operation failed.
 * @note     反复 rt_susp_list_dequeue 直至挂起链为空，用于广播唤醒（如 delete/reset）。
 *
 */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error)
{
    struct rt_thread *thread;

    RT_SCHED_DEBUG_IS_UNLOCKED;

    /* wakeup all suspended threads */
    thread = rt_susp_list_dequeue(susp_list, thread_error);
    while (thread)
    {
        /*
         * resume NEXT thread
         * In rt_thread_resume function, it will remove current thread from
         * suspended list
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error);
    }

    return RT_EOK;
}

/**
 * @brief   This function will resume all suspended threads in the IPC object list,
 *          including the suspended list of IPC object, and private list of mailbox etc.
 *          A lock is passing and hold while operating.
 *
 * @note    This function will resume all threads in the IPC object list.
 *          By contrast, the rt_ipc_list_resume() function will resume a suspended thread in the list of a IPC object.
 *
 * @param   susp_list is a pointer to a suspended thread list of the IPC object.
 * @param   thread_error thread error number of the resuming thread.
 *          A negative value in this set will be discarded, and thread error
 *          will not be changed.
 * @param   lock the lock to be held while operating susp_list
 *
 * @return  Return the operation status. When the return value is RT_EOK, the function is successfully executed.
 *          When the return value is any other values, it means this operation failed.
 *
 */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock)
{
    struct rt_thread *thread;
    rt_base_t level;

    RT_SCHED_DEBUG_IS_UNLOCKED;

    do
    {
        level = rt_spin_lock_irqsave(lock);

        /*
         * resume NEXT thread
         * In rt_thread_resume function, it will remove current thread from
         * suspended list
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error);

        rt_spin_unlock_irqrestore(lock, level);
    }
    while (thread);

    return RT_EOK;
}

/**
 * @brief   Add a thread to the suspend list
 *
 * @note    Caller must hold the scheduler lock
 *
 * @param   susp_list the list thread enqueued to
 * @param   thread the suspended thread
 * @param   ipc_flags the pattern of suspend list
 * @return  RT_EOK on succeed, otherwise a failure
 */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags)
{
    RT_SCHED_DEBUG_IS_LOCKED;

    switch (ipc_flags)
    {
    case RT_IPC_FLAG_FIFO:
        rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread));
        break; /* RT_IPC_FLAG_FIFO */

    case RT_IPC_FLAG_PRIO:
        {
            struct rt_list_node *n;
            struct rt_thread *sthread;

            /* find a suitable position */
            for (n = susp_list->next; n != susp_list; n = n->next)
            {
                sthread = RT_THREAD_LIST_NODE_ENTRY(n);

                /* find out */
                if (rt_sched_thread_get_curr_prio(thread) < rt_sched_thread_get_curr_prio(sthread))
                {
                    /* insert this thread before the sthread */
                    rt_list_insert_before(&RT_THREAD_LIST_NODE(sthread), &RT_THREAD_LIST_NODE(thread));
                    break;
                }
            }

            /*
             * not found a suitable position,
             * append to the end of suspend_thread list
             */
            if (n == susp_list)
                rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread));
        }
        break;/* RT_IPC_FLAG_PRIO */

    default:
        RT_ASSERT(0);
        break;
    }

    return RT_EOK;
}

/**
 * @brief   Print thread on suspend list to system console
 */
void rt_susp_list_print(rt_list_t *list)
{
#ifdef RT_USING_CONSOLE
    rt_sched_lock_level_t slvl;
    struct rt_thread *thread;
    struct rt_list_node *node;

    rt_sched_lock(&slvl);

    for (node = list->next; node != list; node = node->next)
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(node);
        rt_kprintf("%.*s", RT_NAME_MAX, thread->parent.name);

        if (node->next != list)
            rt_kprintf("/");
    }

    rt_sched_unlock(slvl);
#else
    (void)list;
#endif
}

/*===========================================================================*/
/* 信号量（Semaphore）                                                         */
/*                                                                             */
/* 计数信号量：sem->value 表示可用资源个数或「可 P 次数」。take 时减一，release  */
/* 且无阻塞线程时加一（不超过 max_value）。value 初值为 0 时常用作线程间事件   */
/* 通知；初值等于资源池大小时用作资源池令牌。                                   */
/* 二值信号量：可视为 max_value=1 的特例（仍用同一套 API）。                    */
/* 阻塞策略：多线程在 value==0 上竞争时，由 flag（PRIO/FIFO）决定挂起链表顺序。*/
/* 并发保护：本模块对 sem->value 与挂起链表的访问均受 sem->spinlock 保护。    */
/*===========================================================================*/

#ifdef RT_USING_SEMAPHORE
/**
 * @addtogroup group_semaphore Semaphore
 * @{
 */

/* 填充 rt_sem 内存：挂起链表、初值、上限、IPC 排队策略、每信号量自旋锁 */
static void _sem_object_init(rt_sem_t       sem,
                             rt_uint16_t    value,
                             rt_uint8_t     flag,
                             rt_uint16_t    max_value)
{
    /* initialize ipc object */
    _ipc_object_init(&(sem->parent));

    sem->max_value = max_value;
    /* set initial value */
    sem->value = value;

    /* set parent */
    /* flag 传入 rt_thread_suspend_to_list，决定挂起线程按优先级或 FIFO 插入 */
    sem->parent.parent.flag = flag;
    rt_spin_lock_init(&(sem->spinlock));
}

/**
 * @brief    This function will initialize a static semaphore object.
 *
 * @note     For the static semaphore object, its memory space is allocated by the compiler during compiling,
 *           and shall placed on the read-write data segment or on the uninitialized data segment.
 *           By contrast, the rt_sem_create() function will allocate memory space automatically and initialize
 *           the semaphore.
 *
 * @see      rt_sem_create()
 *
 * @param    sem is a pointer to the semaphore to initialize. It is assumed that storage for the semaphore will be
 *           allocated in your application.
 *
 * @param    name is a pointer to the name you would like to give the semaphore.
 *
 * @param    value is the initial value for the semaphore.
 *           If used to share resources, you should initialize the value as the number of available resources.
 *           If used to signal the occurrence of an event, you should initialize the value as 0.
 *
 * @param    flag is the semaphore flag, which determines the queuing way of how multiple threads wait
 *           when the semaphore is not available.
 *           The semaphore flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this semaphore will become non-real-time threads.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it represents the initialization failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     静态信号量，内存由用户分配；max 固定为 RT_SEM_VALUE_MAX。配对使用 rt_sem_detach。
 * @note     value 典型用法：资源池令牌=可用资源数；事件通知则常置 0，由 release 递增值唤醒等待者。
 */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag)
{
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(value < 0x10000U);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* 将对象注册到内核对象容器，类型为信号量 */
    /* initialize object */
    rt_object_init(&(sem->parent.parent), RT_Object_Class_Semaphore, name);

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX);

    return RT_EOK;
}
RTM_EXPORT(rt_sem_init);


/**
 * @brief    This function will detach a static semaphore object.
 *
 * @note     This function is used to detach a static semaphore object which is initialized by rt_sem_init() function.
 *           By contrast, the rt_sem_delete() function will delete a semaphore object.
 *           When the semaphore is successfully detached, it will resume all suspended threads in the semaphore list.
 *
 * @see      rt_sem_delete()
 *
 * @param    sem is a pointer to a semaphore object to be detached.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it means that the semaphore detach failed.
 *
 * @warning  This function can ONLY detach a static semaphore initialized by the rt_sem_init() function.
 *           If the semaphore is created by the rt_sem_create() function, you MUST NOT USE this function to detach it,
 *           ONLY USE the rt_sem_delete() function to complete the deletion.
 * @note     唤醒挂起线程时置 error 为 RT_ERROR，表示对象已失效；再从容器摘除，不释放 sem 存储本身。
 */
rt_err_t rt_sem_detach(rt_sem_t sem)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent));

    level = rt_spin_lock_irqsave(&(sem->spinlock));
    /* wakeup all suspended threads */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* 仅从内核对象链表 detach，静态对象内存由用户管理 */
    /* detach semaphore object */
    rt_object_detach(&(sem->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_sem_detach);

#ifdef RT_USING_HEAP
/**
 * @brief    Creating a semaphore object.
 *
 * @note     For the semaphore object, its memory space is allocated automatically.
 *           By contrast, the rt_sem_init() function will initialize a static semaphore object.
 *
 * @see      rt_sem_init()
 *
 * @param    name is a pointer to the name you would like to give the semaphore.
 *
 * @param    value is the initial value for the semaphore.
 *           If used to share resources, you should initialize the value as the number of available resources.
 *           If used to signal the occurrence of an event, you should initialize the value as 0.
 *
 * @param    flag is the semaphore flag, which determines the queuing way of how multiple threads wait
 *           when the semaphore is not available.
 *           The semaphore flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this semaphore will become non-real-time threads.
 *
 * @return   Return a pointer to the semaphore object. When the return value is RT_NULL, it means the creation failed.
 *
 * @warning  This function can NOT be called in interrupt context. You can use macor RT_DEBUG_NOT_IN_INTERRUPT to check it.
 * @note     动态信号量，对象与内存在堆上；配对 rt_sem_delete。不可在 ISR 中调用。
 */
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    rt_sem_t sem;

    RT_ASSERT(value < 0x10000U);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* allocate object */
    sem = (rt_sem_t)rt_object_allocate(RT_Object_Class_Semaphore, name);
    if (sem == RT_NULL)
        return sem;

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX);

    return sem;
}
RTM_EXPORT(rt_sem_create);


/**
 * @brief    This function will delete a semaphore object and release the memory space.
 *
 * @note     This function is used to delete a semaphore object which is created by the rt_sem_create() function.
 *           By contrast, the rt_sem_detach() function will detach a static semaphore object.
 *           When the semaphore is successfully deleted, it will resume all suspended threads in the semaphore list.
 *
 * @see      rt_sem_detach()
 *
 * @param    sem is a pointer to a semaphore object to be deleted.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the semaphore detach failed.
 *
 * @warning  This function can ONLY delete a semaphore initialized by the rt_sem_create() function.
 *           If the semaphore is initialized by the rt_sem_init() function, you MUST NOT USE this function to delete it,
 *           ONLY USE the rt_sem_detach() function to complete the detachment.
 * @note     rt_object_delete 会释放堆上的信号量控制块；与 rt_sem_create 严格配对。
 */
rt_err_t rt_sem_delete(rt_sem_t sem)
{
    rt_ubase_t level;

    /* parameter check */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    level = rt_spin_lock_irqsave(&(sem->spinlock));
    /* wakeup all suspended threads */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* 释放对象及关联堆内存 */
    /* delete semaphore object */
    rt_object_delete(&(sem->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_sem_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief    This function will take a semaphore, if the semaphore is unavailable, the thread shall wait for
 *           the semaphore up to a specified time.
 *
 * @note     When this function is called, the count value of the sem->value will decrease 1 until it is equal to 0.
 *           When the sem->value is 0, it means that the semaphore is unavailable. At this time, it will suspend the
 *           thread preparing to take the semaphore.
 *           On the contrary, the rt_sem_release() function will increase the count value of sem->value by 1 each time.
 *
 * @see      rt_sem_trytake()
 *
 * @param    sem is a pointer to a semaphore object.
 *
 * @param    timeout is a timeout period (unit: an OS tick). If the semaphore is unavailable, the thread will wait for
 *           the semaphore up to the amount of time specified by this parameter.
 *
 *           NOTE:
 *           If use Macro RT_WAITING_FOREVER to set this parameter, which means that when the
 *           message is unavailable in the queue, the thread will be waiting forever.
 *           If use macro RT_WAITING_NO to set this parameter, which means that this
 *           function is non-blocking and will return immediately.
 *
 * @return   Return the operation status. ONLY When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the semaphore take failed.
 *
 * @warning  This function can ONLY be called in the thread context. It MUST NOT BE called in interrupt context.
 * @note     中文（实现体 _rt_sem_take）：timeout 为 tick，RT_WAITING_FOREVER(-1) 永久等待，0 即非阻塞 try，
 *           正数为限时 tick。suspend_flag 传入 rt_thread_suspend_to_list，区分 UNINTERRUPTIBLE / INTERRUPTIBLE /
 *           KILLABLE。流程：持锁若 value 大于 0 则减一并返回；否则 timeout==0 返回 -RT_ETIMEOUT；否则挂入
 *           sem 的挂起链表，timeout 为正时启线程定时器，解锁后 rt_schedule，唤醒后据 thread->error 返回。
 */
static rt_err_t _rt_sem_take(rt_sem_t sem, rt_int32_t timeout, int suspend_flag)
{
    rt_base_t level;
    struct rt_thread *thread;
    rt_err_t ret;

    /* parameter check */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(sem->parent.parent)));

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(1);

    level = rt_spin_lock_irqsave(&(sem->spinlock));

    LOG_D("thread %s take sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value);

    if (sem->value > 0)
    {
        /* semaphore is available */
        /* 有可用计数：直接减一并成功返回（典型快速路径） */
        sem->value --;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);
    }
    else
    {
        /* no waiting, return with timeout */
        if (timeout == 0)
        {
            /* 非阻塞：等同 rt_sem_trytake */
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);
            return -RT_ETIMEOUT;
        }
        else
        {
            /* semaphore is unavailable, push to suspend list */
            /* get current thread */
            thread = rt_thread_self();

            /* reset thread error number */
            thread->error = RT_EINTR;

            LOG_D("sem take: suspend thread - %s", thread->parent.name);

            /* suspend thread */
            /* 按 sem 上记录的 PRIO/FIFO 策略插入挂起链表 */
            ret = rt_thread_suspend_to_list(thread, &(sem->parent.suspend_thread),
                                            sem->parent.parent.flag, suspend_flag);
            if (ret != RT_EOK)
            {
                rt_spin_unlock_irqrestore(&(sem->spinlock), level);
                return ret;
            }

            /* has waiting time, start thread timer */
            if (timeout > 0)
            {
                LOG_D("set thread:%s to timer list", thread->parent.name);

                /* reset the timeout of thread timer and start it */
                /* RT_WAITING_FOREVER 时不启定时器，依赖 release 唤醒 */
                rt_timer_control(&(thread->thread_timer),
                                 RT_TIMER_CTRL_SET_TIME,
                                 &timeout);
                rt_timer_start(&(thread->thread_timer));
            }

            /* enable interrupt */
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);

            /* do schedule */
            /* 主动放弃 CPU，直至被 release/超时/信号等路径就绪 */
            rt_schedule();

            if (thread->error != RT_EOK)
            {
                return thread->error > 0 ? -thread->error : thread->error;
            }
        }
    }

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(sem->parent.parent)));

    return RT_EOK;
}

/* 默认 take：挂起期间不可被信号打断（RT_UNINTERRUPTIBLE） */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take);

/* 可被信号打断的阻塞 take，适用于需响应 POSIX 信号等场景 */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take_interruptible);

/* 可被「杀死」类操作打断的阻塞 take（与线程取消语义相关，依内核配置） */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_KILLABLE);
}
RTM_EXPORT(rt_sem_take_killable);

/**
 * @brief    This function will try to take a semaphore, if the semaphore is unavailable, the thread returns immediately.
 *
 * @note     This function is very similar to the rt_sem_take() function, when the semaphore is not available,
 *           the rt_sem_trytake() function will return immediately without waiting for a timeout.
 *           In other words, rt_sem_trytake(sem) has the same effect as rt_sem_take(sem, 0).
 *
 * @see      rt_sem_take()
 *
 * @param    sem is a pointer to a semaphore object.
 *
 * @return   Return the operation status. ONLY When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the semaphore take failed.
 * @note     内部即 rt_sem_take(sem, 0)；无计数时立即返回 -RT_ETIMEOUT，永不阻塞。
 */
rt_err_t rt_sem_trytake(rt_sem_t sem)
{
    return rt_sem_take(sem, RT_WAITING_NO);
}
RTM_EXPORT(rt_sem_trytake);


/**
 * @brief    This function will release a semaphore. If there is thread suspended on the semaphore, it will get resumed.
 *
 * @note     If there are threads suspended on this semaphore, the first thread in the list of this semaphore object
 *           will be resumed, and a thread scheduling (rt_schedule) will be executed.
 *           If no threads are suspended on this semaphore, the count value sem->value of this semaphore will increase by 1.
 *
 * @param    sem is a pointer to a semaphore object.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the semaphore release failed.
 * @note     V 操作。若有阻塞线程则唤醒队首一个（不增加 value）；否则 value++，已达 max 返回 -RT_EFULL。
 *           可在 ISR 中调用（无阻塞路径），唤醒后按需 rt_schedule。
 */
rt_err_t rt_sem_release(rt_sem_t sem)
{
    rt_base_t level;
    rt_bool_t need_schedule;

    /* parameter check */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(sem->parent.parent)));

    need_schedule = RT_FALSE;

    level = rt_spin_lock_irqsave(&(sem->spinlock));

    LOG_D("thread %s releases sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value);

    if (!rt_list_isempty(&sem->parent.suspend_thread))
    {
        /* resume the suspended thread */
        /* 优先把资源交给已等待线程：被唤醒线程在就绪路径上完成「等效 take」，此处不增 value */
        rt_susp_list_dequeue(&(sem->parent.suspend_thread), RT_EOK);
        need_schedule = RT_TRUE;
    }
    else
    {
        /* 无等待者：累积可用计数，但不能超过创建/配置时的上限 */
        if(sem->value < sem->max_value)
        {
            sem->value ++; /* increase value */
        }
        else
        {
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);
            return -RT_EFULL; /* value overflowed */
        }
    }

    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* resume a thread, re-schedule */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_sem_release);


/**
 * @brief    This function will set some extra attributions of a semaphore object.
 *
 * @note     Currently this function only supports the RT_IPC_CMD_RESET command to reset the semaphore.
 *
 * @param    sem is a pointer to a semaphore object.
 *
 * @param    cmd is a command word used to configure some attributions of the semaphore.
 *
 * @param    arg is the argument of the function to execute the command.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that this function failed to execute.
 * @note     RT_IPC_CMD_RESET 清空等待并设 value=(rt_uintptr_t)arg；RT_IPC_CMD_SET_VLIMIT 调整 max_value，
 *           若新上限小于当前 value 且仍有阻塞线程则全部唤醒并报错。
 */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    if (cmd == RT_IPC_CMD_RESET)
    {
        rt_ubase_t value;

        /* get value */
        value = (rt_uintptr_t)arg;
        level = rt_spin_lock_irqsave(&(sem->spinlock));

        /* resume all waiting thread */
        /* 强制重置：所有阻塞方失败返回，再写入新初值 */
        rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);

        /* set new value */
        sem->value = (rt_uint16_t)value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);
        rt_schedule();

        return RT_EOK;
    }
    else if (cmd == RT_IPC_CMD_SET_VLIMIT)
    {
        rt_ubase_t max_value;
        rt_bool_t need_schedule = RT_FALSE;

        max_value = (rt_uint16_t)((rt_uintptr_t)arg);
        if (max_value > RT_SEM_VALUE_MAX || max_value < 1)
        {
            return -RT_EINVAL;
        }

        level = rt_spin_lock_irqsave(&(sem->spinlock));
        /* 缩小上限且当前计数已超过新上限时，无法保留原语义，唤醒所有等待者由应用重试 */
        if (max_value < sem->value)
        {
            if (!rt_list_isempty(&sem->parent.suspend_thread))
            {
                /* resume all waiting thread */
                rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);
                need_schedule = RT_TRUE;
            }
        }
        /* set new value */
        sem->max_value = max_value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);

        if (need_schedule)
        {
            rt_schedule();
        }

        return RT_EOK;
    }

    return -RT_ERROR;
}
RTM_EXPORT(rt_sem_control);

/**@}*/
#endif /* RT_USING_SEMAPHORE */

/*===========================================================================*/
/* 互斥量（Mutex）                                                            */
/*                                                                             */
/* 互斥锁：同一时刻最多一个线程为 owner；同一线程可递归 take（hold 计数）。   */
/* 优先级继承：高优先级线程阻塞在 mutex 上时，临时提升 owner 优先级，避免优先级 */
/* 反转；释放后按 taken_object_list 中仍持有的 mutex 重新计算有效优先级。   */
/* 优先级天花板：ceiling_priority 有效时，owner 在持锁期间优先级被抬到天花板。 */
/* 等待队列强制 RT_IPC_FLAG_PRIO（init/create 中写死），FIFO 无法解决无界反转。*/
/* 并发：mutex->spinlock + 多处配合 rt_sched_lock 保护 owner/hold/挂起链表。  */
/*===========================================================================*/

#ifdef RT_USING_MUTEX
/* 根据挂起链表队首线程更新 mutex->priority（阻塞在此 mutex 上的最高优先级） */
/* iterate over each suspended thread to update highest priority in pending threads */
rt_inline rt_uint8_t _mutex_update_priority(struct rt_mutex *mutex)
{
    struct rt_thread *thread;

    if (!rt_list_isempty(&mutex->parent.suspend_thread))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
        mutex->priority = rt_sched_thread_get_curr_prio(thread);
    }
    else
    {
        mutex->priority = 0xff;
    }

    return mutex->priority;
}

/* 计算线程因「已持有的多个 mutex」应处的有效优先级：取 init_prio 与各 mutex 上 */
/* mutex->priority 与 ceiling 的较小者再取 min（数值越小优先级越高） */
/* get highest priority inside its taken object and its init priority */
rt_inline rt_uint8_t _thread_get_mutex_priority(struct rt_thread* thread)
{
    rt_list_t *node = RT_NULL;
    struct rt_mutex *mutex = RT_NULL;
    rt_uint8_t priority = rt_sched_thread_get_init_prio(thread);

    rt_list_for_each(node, &(thread->taken_object_list))
    {
        mutex = rt_list_entry(node, struct rt_mutex, taken_list);
        rt_uint8_t mutex_prio = mutex->priority;
        /* prio at least be priority ceiling */
        mutex_prio = mutex_prio < mutex->ceiling_priority ? mutex_prio : mutex->ceiling_priority;

        if (priority > mutex_prio)
        {
            priority = mutex_prio;
        }
    }

    return priority;
}

/* 将 thread 调度优先级改为 priority；若其仍挂起在另一 mutex 上，则沿 pending 链继续 */
/* 提升（或调整队列顺序）mutex 持有者，形成优先级继承链 */
/* update priority of target thread and the thread suspended it if any */
rt_inline void _thread_update_priority(struct rt_thread *thread, rt_uint8_t priority, int suspend_flag)
{
    rt_err_t ret = -RT_ERROR;
    struct rt_object* pending_obj = RT_NULL;

    LOG_D("thread:%s priority -> %d", thread->parent.name, priority);

    /* change priority of the thread */
    ret = rt_sched_thread_change_priority(thread, priority);

    while ((ret == RT_EOK) && rt_sched_thread_is_suspended(thread))
    {
        /* whether change the priority of taken mutex */
        pending_obj = thread->pending_object;

        if (pending_obj && rt_object_get_type(pending_obj) == RT_Object_Class_Mutex)
        {
            rt_uint8_t mutex_priority = 0xff;
            struct rt_mutex* pending_mutex = (struct rt_mutex *)pending_obj;

            /* re-insert thread to suspended thread list to resort priority list */
            rt_list_remove(&RT_THREAD_LIST_NODE(thread));

            ret = rt_susp_list_enqueue(
                &(pending_mutex->parent.suspend_thread), thread,
                pending_mutex->parent.parent.flag);
            if (ret == RT_EOK)
            {
                /* update priority */
                _mutex_update_priority(pending_mutex);
                /* change the priority of mutex owner thread */
                LOG_D("mutex: %s priority -> %d", pending_mutex->parent.parent.name,
                        pending_mutex->priority);

                mutex_priority = _thread_get_mutex_priority(pending_mutex->owner);
                if (mutex_priority != rt_sched_thread_get_curr_prio(pending_mutex->owner))
                {
                    thread = pending_mutex->owner;

                    ret = rt_sched_thread_change_priority(thread, mutex_priority);
                }
                else
                {
                    ret = -RT_ERROR;
                }
            }
        }
        else
        {
            ret = -RT_ERROR;
        }
    }
}

/* 释放/删除 mutex 后：若曾用天花板或继承把 owner 抬到 mutex->priority，则按 taken 链表重算优先级 */
static rt_bool_t _check_and_update_prio(rt_thread_t thread, rt_mutex_t mutex)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    rt_bool_t do_sched = RT_FALSE;

    if ((mutex->ceiling_priority != 0xFF) || (rt_sched_thread_get_curr_prio(thread) == mutex->priority))
    {
        rt_uint8_t priority = 0xff;

        /* get the highest priority in the taken list of thread */
        priority = _thread_get_mutex_priority(thread);

        rt_sched_thread_change_priority(thread, priority);

        /**
         * notify a pending reschedule. Since scheduler is locked, we will not
         * really do a re-schedule at this point
         */
        do_sched = RT_TRUE;
    }
    return do_sched;
}

/* delete/detach 前：唤醒所有阻塞者，从 owner 的 taken 链摘掉本 mutex，必要时下调 owner 优先级 */
static void _mutex_before_delete_detach(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl;
    rt_bool_t need_schedule = RT_FALSE;

    rt_spin_lock(&(mutex->spinlock));
    /* wakeup all suspended threads */
    rt_susp_list_resume_all(&(mutex->parent.suspend_thread), RT_ERROR);

    rt_sched_lock(&slvl);

    /* remove mutex from thread's taken list */
    rt_list_remove(&mutex->taken_list);

    /* whether change the thread priority */
    if (mutex->owner)
    {
        need_schedule = _check_and_update_prio(mutex->owner, mutex);
    }

    if (need_schedule)
    {
        rt_sched_unlock_n_resched(slvl);
    }
    else
    {
        rt_sched_unlock(slvl);
    }

    /* unlock and do necessary reschedule if required */
    rt_spin_unlock(&(mutex->spinlock));
}

/**
 * @addtogroup group_mutex Mutex
 * @{
 */

/**
 * @brief    Initialize a static mutex object.
 *
 * @note     For the static mutex object, its memory space is allocated by the compiler during compiling,
 *           and shall placed on the read-write data segment or on the uninitialized data segment.
 *           By contrast, the rt_mutex_create() function will automatically allocate memory space
 *           and initialize the mutex.
 *
 * @see      rt_mutex_create()
 *
 * @param    mutex is a pointer to the mutex to initialize. It is assumed that storage for the mutex will be
 *           allocated in your application.
 *
 * @param    name is a pointer to the name that given to the mutex.
 *
 * @param    flag is the mutex flag, which determines the queuing way of how multiple threads wait
 *           when the mutex is not available.
 *           NOTE: This parameter has been obsoleted. It can be RT_IPC_FLAG_PRIO, RT_IPC_FLAG_FIFO or RT_NULL.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it represents the initialization failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     静态互斥量；flag 参数已废弃。owner/hold 表递归，ceiling_priority 默认 0xFF 表示未启用天花板。
 * @note     配对 rt_mutex_detach；递归深度受 RT_MUTEX_HOLD_MAX 限制。
 */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag)
{
    /* flag parameter has been obsoleted */
    RT_UNUSED(flag);

    /* parameter check */
    RT_ASSERT(mutex != RT_NULL);

    /* initialize object */
    rt_object_init(&(mutex->parent.parent), RT_Object_Class_Mutex, name);

    /* initialize ipc object */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL;
    mutex->priority = 0xFF;
    mutex->hold     = 0;
    mutex->ceiling_priority = 0xFF;
    /* taken_list 作为节点挂入 thread->taken_object_list，用于嵌套 mutex 时重算优先级 */
    rt_list_init(&(mutex->taken_list));

    /* flag can only be RT_IPC_FLAG_PRIO. RT_IPC_FLAG_FIFO cannot solve the unbounded priority inversion problem */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_init);


/**
 * @brief    This function will detach a static mutex object.
 *
 * @note     This function is used to detach a static mutex object which is initialized by rt_mutex_init() function.
 *           By contrast, the rt_mutex_delete() function will delete a mutex object.
 *           When the mutex is successfully detached, it will resume all suspended threads in the mutex list.
 *
 * @see      rt_mutex_delete()
 *
 * @param    mutex is a pointer to a mutex object to be detached.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it means that the mutex detach failed.
 *
 * @warning  This function can ONLY detach a static mutex initialized by the rt_mutex_init() function.
 *           If the mutex is created by the rt_mutex_create() function, you MUST NOT USE this function to detach it,
 *           ONLY USE the rt_mutex_delete() function to complete the deletion.
 * @note     先 _mutex_before_delete_detach 再 detach；不释放静态 mutex 存储。
 */
rt_err_t rt_mutex_detach(rt_mutex_t mutex)
{
    /* parameter check */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent));

    _mutex_before_delete_detach(mutex);

    /* detach mutex object */
    rt_object_detach(&(mutex->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_detach);

/* drop a thread from the suspend list of mutex */

/**
 * @brief drop a thread from the suspend list of mutex
 *
 * @param mutex is a pointer to a mutex object.
 * @param thread is the thread should be dropped from mutex.
 * @note     将 thread 从 mutex 挂起链摘除（如超时、信号打断）；重算 mutex->priority 并可能下调 owner。
 */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread)
{
    rt_uint8_t priority;
    rt_bool_t need_update = RT_FALSE;
    rt_sched_lock_level_t slvl;

    /* parameter check */
    RT_DEBUG_IN_THREAD_CONTEXT;
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(thread != RT_NULL);

    rt_spin_lock(&(mutex->spinlock));

    RT_ASSERT(thread->pending_object == &mutex->parent.parent);

    rt_sched_lock(&slvl);

    /* detach from suspended list */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));

    /**
     * Should change the priority of mutex owner thread
     * Note: After current thread is detached from mutex pending list, there is
     *       a chance that the mutex owner has been released the mutex. Which
     *       means mutex->owner can be NULL at this point. If that happened,
     *       it had already reset its priority. So it's okay to skip
     */
    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) ==
                            rt_sched_thread_get_curr_prio(thread))
    {
        need_update = RT_TRUE;
    }

    /* update the priority of mutex */
    if (!rt_list_isempty(&mutex->parent.suspend_thread))
    {
        /* more thread suspended in the list */
        struct rt_thread *th;

        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
        /* update the priority of mutex */
        mutex->priority = rt_sched_thread_get_curr_prio(th);
    }
    else
    {
        /* set mutex priority to maximal priority */
        mutex->priority = 0xff;
    }

    /* try to change the priority of mutex owner thread */
    if (need_update)
    {
        /* get the maximal priority of mutex in thread */
        priority = _thread_get_mutex_priority(mutex->owner);
        if (priority != rt_sched_thread_get_curr_prio(mutex->owner))
        {
            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE);
        }
    }

    rt_sched_unlock(slvl);
    rt_spin_unlock(&(mutex->spinlock));
}


/**
 * @brief set the prioceiling attribute of the mutex.
 *
 * @param mutex is a pointer to a mutex object.
 * @param priority is the priority should be set to mutex.
 *
 * @return return the old priority ceiling
 * @note     设置天花板优先级并返回旧值；若已有 owner，立即按新天花板重算其有效优先级。
 */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority)
{
    rt_uint8_t ret_priority = 0xFF;
    rt_uint8_t highest_prio;
    rt_sched_lock_level_t slvl;

    RT_DEBUG_IN_THREAD_CONTEXT;

    if ((mutex) && (priority < RT_THREAD_PRIORITY_MAX))
    {
        /* critical section here if multiple updates to one mutex happen */
        rt_spin_lock(&(mutex->spinlock));
        ret_priority = mutex->ceiling_priority;
        mutex->ceiling_priority = priority;
        if (mutex->owner)
        {
            rt_sched_lock(&slvl);
            highest_prio = _thread_get_mutex_priority(mutex->owner);
            if (highest_prio != rt_sched_thread_get_curr_prio(mutex->owner))
            {
                _thread_update_priority(mutex->owner, highest_prio, RT_UNINTERRUPTIBLE);
            }
            rt_sched_unlock(slvl);
        }
        rt_spin_unlock(&(mutex->spinlock));
    }
    else
    {
        rt_set_errno(-RT_EINVAL);
    }

    return ret_priority;
}
RTM_EXPORT(rt_mutex_setprioceiling);


/**
 * @brief set the prioceiling attribute of the mutex.
 *
 * @param mutex is a pointer to a mutex object.
 *
 * @return return the current priority ceiling of the mutex.
 * @note     0xFF 表示未设置天花板；读操作在 spinlock 内完成。
 */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex)
{
    rt_uint8_t prio = 0xFF;

    /* parameter check */
    RT_DEBUG_IN_THREAD_CONTEXT;
    RT_ASSERT(mutex != RT_NULL);

    if (mutex)
    {
        rt_spin_lock(&(mutex->spinlock));
        prio = mutex->ceiling_priority;
        rt_spin_unlock(&(mutex->spinlock));
    }

    return prio;
}
RTM_EXPORT(rt_mutex_getprioceiling);


#ifdef RT_USING_HEAP
/**
 * @brief    This function will create a mutex object.
 *
 * @note     For the mutex object, its memory space is automatically allocated.
 *           By contrast, the rt_mutex_init() function will initialize a static mutex object.
 *
 * @see      rt_mutex_init()
 *
 * @param    name is a pointer to the name that given to the mutex.
 *
 * @param    flag is the mutex flag, which determines the queuing way of how multiple threads wait
 *           when the mutex is not available.
 *           NOTE: This parameter has been obsoleted. It can be RT_IPC_FLAG_PRIO, RT_IPC_FLAG_FIFO or RT_NULL.
 *
 * @return   Return a pointer to the mutex object. When the return value is RT_NULL, it means the creation failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     堆上动态创建，配对 rt_mutex_delete；不可在 ISR 中调用。
 */
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag)
{
    struct rt_mutex *mutex;

    /* flag parameter has been obsoleted */
    RT_UNUSED(flag);

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* allocate object */
    mutex = (rt_mutex_t)rt_object_allocate(RT_Object_Class_Mutex, name);
    if (mutex == RT_NULL)
        return mutex;

    /* initialize ipc object */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL;
    mutex->priority = 0xFF;
    mutex->hold     = 0;
    mutex->ceiling_priority = 0xFF;
    rt_list_init(&(mutex->taken_list));

    /* flag can only be RT_IPC_FLAG_PRIO. RT_IPC_FLAG_FIFO cannot solve the unbounded priority inversion problem */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock));

    return mutex;
}
RTM_EXPORT(rt_mutex_create);


/**
 * @brief    This function will delete a mutex object and release this memory space.
 *
 * @note     This function is used to delete a mutex object which is created by the rt_mutex_create() function.
 *           By contrast, the rt_mutex_detach() function will detach a static mutex object.
 *           When the mutex is successfully deleted, it will resume all suspended threads in the mutex list.
 *
 * @see      rt_mutex_detach()
 *
 * @param    mutex is a pointer to a mutex object to be deleted.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mutex detach failed.
 *
 * @warning  This function can ONLY delete a mutex initialized by the rt_mutex_create() function.
 *           If the mutex is initialized by the rt_mutex_init() function, you MUST NOT USE this function to delete it,
 *           ONLY USE the rt_mutex_detach() function to complete the detachment.
 * @note     rt_object_delete 释放堆内存；与 rt_mutex_create 严格配对。
 */
rt_err_t rt_mutex_delete(rt_mutex_t mutex)
{
    /* parameter check */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    _mutex_before_delete_detach(mutex);

    /* delete mutex object */
    rt_object_delete(&(mutex->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief    This function will take a mutex, if the mutex is unavailable, the thread shall wait for
 *           the mutex up to a specified time.
 *
 * @note     When this function is called, the count value of the mutex->value will decrease 1 until it is equal to 0.
 *           When the mutex->value is 0, it means that the mutex is unavailable. At this time, it will suspend the
 *           thread preparing to take the mutex.
 *           On the contrary, the rt_mutex_release() function will increase the count value of mutex->value by 1 each time.
 * @note     中文（与实现一致）：互斥量用 owner 与 hold 表示占用，非上述 value 模型。同线程重复 take 仅增加 hold；
 *           无 owner 时直接获得锁并可应用优先级天花板；已被占用则挂起，必要时提升 owner 优先级（继承）。
 *           唤醒成功路径要求 thread->error==RT_EOK。
 *
 * @see      rt_mutex_trytake()
 *
 * @param    mutex is a pointer to a mutex object.
 *
 * @param    timeout is a timeout period (unit: an OS tick). If the mutex is unavailable, the thread will wait for
 *           the mutex up to the amount of time specified by the argument.
 *           NOTE: Generally, we set this parameter to RT_WAITING_FOREVER, which means that when the mutex is unavailable,
 *           the thread will be waitting forever.
 *
 * @return   Return the operation status. ONLY When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mutex take failed.
 *
 * @warning  This function can ONLY be called in the thread context. It MUST NOT BE called in interrupt context.
 */
static rt_err_t _rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread;
    rt_err_t ret;

    /* this function must not be used in interrupt even if time = 0 */
    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    /* parameter check */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);

    /* get current thread */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock));

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mutex->parent.parent)));

    LOG_D("mutex_take: current thread %s, hold: %d",
          thread->parent.name, mutex->hold);

    /* reset thread error */
    thread->error = RT_EOK;

    if (mutex->owner == thread)
    {
        if (mutex->hold < RT_MUTEX_HOLD_MAX)
        {
            /* it's the same thread */
            /* 递归加锁：同一线程再次 take，仅增加 hold，不做优先级继承 */
            mutex->hold ++;
        }
        else
        {
            rt_spin_unlock(&(mutex->spinlock));
            return -RT_EFULL; /* value overflowed */
        }
    }
    else
    {
        /* whether the mutex has owner thread. */
        if (mutex->owner == RT_NULL)
        {
            /* set mutex owner and original priority */
            /* 空闲：当前线程成为 owner，hold=1，mutex 节点加入 taken 链表 */
            mutex->owner    = thread;
            mutex->priority = 0xff;
            mutex->hold     = 1;

            if (mutex->ceiling_priority != 0xFF)
            {
                /* set the priority of thread to the ceiling priority */
                /* 已设置天花板且低于当前运行优先级时，把 owner 抬到天花板 */
                if (mutex->ceiling_priority < rt_sched_thread_get_curr_prio(mutex->owner))
                    _thread_update_priority(mutex->owner, mutex->ceiling_priority, suspend_flag);
            }

            /* insert mutex to thread's taken object list */
            rt_list_insert_after(&thread->taken_object_list, &mutex->taken_list);
        }
        else
        {
            /* no waiting, return with timeout */
            if (timeout == 0)
            {
                /* set error as timeout */
                thread->error = RT_ETIMEOUT;

                rt_spin_unlock(&(mutex->spinlock));
                return -RT_ETIMEOUT;
            }
            else
            {
                rt_sched_lock_level_t slvl;
                rt_uint8_t priority;

                /* mutex is unavailable, push to suspend list */
                LOG_D("mutex_take: suspend thread: %s",
                      thread->parent.name);

                /* suspend current thread */
                ret = rt_thread_suspend_to_list(thread, &(mutex->parent.suspend_thread),
                                                mutex->parent.parent.flag, suspend_flag);
                if (ret != RT_EOK)
                {
                    rt_spin_unlock(&(mutex->spinlock));
                    return ret;
                }

                /* set pending object in thread to this mutex */
                thread->pending_object = &(mutex->parent.parent);

                rt_sched_lock(&slvl);

                priority = rt_sched_thread_get_curr_prio(thread);

                /* update the priority level of mutex */
                /* mutex->priority 记录阻塞者中最高优先级，用于决定是否继承给 owner */
                if (priority < mutex->priority)
                {
                    mutex->priority = priority;
                    if (mutex->priority < rt_sched_thread_get_curr_prio(mutex->owner))
                    {
                        _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE); /* TODO */
                    }
                }

                rt_sched_unlock(slvl);

                /* has waiting time, start thread timer */
                if (timeout > 0)
                {
                    LOG_D("mutex_take: start the timer of thread:%s",
                          thread->parent.name);

                    /* reset the timeout of thread timer and start it */
                    rt_timer_control(&(thread->thread_timer),
                                     RT_TIMER_CTRL_SET_TIME,
                                     &timeout);
                    rt_timer_start(&(thread->thread_timer));
                }

                rt_spin_unlock(&(mutex->spinlock));

                /* do schedule */
                rt_schedule();

                rt_spin_lock(&(mutex->spinlock));

                if (mutex->owner == thread)
                {
                    /**
                     * get mutex successfully
                     * Note: assert to avoid an unexpected resume
                     */
                    /* 被 release 选中成为新 owner */
                    RT_ASSERT(thread->error == RT_EOK);
                }
                else
                {
                    /* the mutex has not been taken and thread has detach from the pending list. */
                    /* 超时/打断等：未获得锁，需恢复 mutex 继承状态并返回错误码 */

                    rt_bool_t need_update = RT_FALSE;
                    RT_ASSERT(mutex->owner != thread);

                    /* get value first before calling to other APIs */
                    ret = thread->error;

                    /* unexpected resume */
                    if (ret == RT_EOK)
                    {
                        ret = -RT_EINTR;
                    }

                    rt_sched_lock(&slvl);

                    /**
                     * Should change the priority of mutex owner thread
                     * Note: After current thread is detached from mutex pending list, there is
                     *       a chance that the mutex owner has been released the mutex. Which
                     *       means mutex->owner can be NULL at this point. If that happened,
                     *       it had already reset its priority. So it's okay to skip
                     */
                    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) == rt_sched_thread_get_curr_prio(thread))
                        need_update = RT_TRUE;

                    /* update the priority of mutex */
                    if (!rt_list_isempty(&mutex->parent.suspend_thread))
                    {
                        /* more thread suspended in the list */
                        struct rt_thread *th;

                        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
                        /* update the priority of mutex */
                        mutex->priority = rt_sched_thread_get_curr_prio(th);
                    }
                    else
                    {
                        /* set mutex priority to maximal priority */
                        mutex->priority = 0xff;
                    }

                    /* try to change the priority of mutex owner thread */
                    if (need_update)
                    {
                        /* get the maximal priority of mutex in thread */
                        priority = _thread_get_mutex_priority(mutex->owner);
                        if (priority != rt_sched_thread_get_curr_prio(mutex->owner))
                        {
                            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE);
                        }
                    }

                    rt_sched_unlock(slvl);

                    rt_spin_unlock(&(mutex->spinlock));

                    /* clear pending object before exit */
                    thread->pending_object = RT_NULL;

                    /* fix thread error number to negative value and return */
                    return ret > 0 ? -ret : ret;
                }
            }
        }
    }

    rt_spin_unlock(&(mutex->spinlock));

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mutex->parent.parent)));

    return RT_EOK;
}

/* 默认 RT_UNINTERRUPTIBLE：阻塞等待期间不因信号等退出（与 interruptible / killable 变体相对） */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take);

rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take_interruptible);

rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_KILLABLE);
}
RTM_EXPORT(rt_mutex_take_killable);

/**
 * @brief    This function will try to take a mutex, if the mutex is unavailable, the thread returns immediately.
 *
 * @note     This function is very similar to the rt_mutex_take() function, when the mutex is not available,
 *           except that rt_mutex_trytake() will return immediately without waiting for a timeout
 *           when the mutex is not available.
 *           In other words, rt_mutex_trytake(mutex) has the same effect as rt_mutex_take(mutex, 0).
 *
 * @see      rt_mutex_take()
 *
 * @param    mutex is a pointer to a mutex object.
 *
 * @return   Return the operation status. ONLY When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mutex take failed.
 * @note     等价 rt_mutex_take(mutex, 0)；已被占用立即 -RT_ETIMEOUT。
 */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex)
{
    return rt_mutex_take(mutex, RT_WAITING_NO);
}
RTM_EXPORT(rt_mutex_trytake);


/**
 * @brief    This function will release a mutex. If there is thread suspended on the mutex, the thread will be resumed.
 *
 * @note     If there are threads suspended on this mutex, the first thread in the list of this mutex object
 *           will be resumed, and a thread scheduling (rt_schedule) will be executed.
 *           If no threads are suspended on this mutex, the count value mutex->value of this mutex will increase by 1.
 * @note     仅 owner 可 release；每 release 一层 hold--，到 0 才真正释放锁。有等待者时直接把 mutex
 *           交给队首线程（其 hold=1），并更新继承优先级；无等待者则 owner=NULL。非 owner 返回 -RT_ERROR。
 *
 * @param    mutex is a pointer to a mutex object.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mutex release failed.
 */
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl;
    struct rt_thread *thread;
    rt_bool_t need_schedule;

    /* parameter check */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);

    need_schedule = RT_FALSE;

    /* only thread could release mutex because we need test the ownership */
    RT_DEBUG_IN_THREAD_CONTEXT;

    /* get current thread */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock));

    LOG_D("mutex_release:current thread %s, hold: %d",
          thread->parent.name, mutex->hold);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mutex->parent.parent)));

    /* mutex only can be released by owner */
    if (thread != mutex->owner)
    {
        thread->error = -RT_ERROR;
        rt_spin_unlock(&(mutex->spinlock));

        return -RT_ERROR;
    }

    /* decrease hold */
    mutex->hold --;
    /* if no hold */
    /* 最外层配对：此时才真正释放互斥资源 */
    if (mutex->hold == 0)
    {
        rt_sched_lock(&slvl);

        /* remove mutex from thread's taken list */
        rt_list_remove(&mutex->taken_list);

        /* whether change the thread priority */
        need_schedule = _check_and_update_prio(thread, mutex);

        /* wakeup suspended thread */
        if (!rt_list_isempty(&mutex->parent.suspend_thread))
        {
            struct rt_thread *next_thread;
            do
            {
                /* get the first suspended thread */
                next_thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);

                RT_ASSERT(rt_sched_thread_is_suspended(next_thread));

                /* remove the thread from the suspended list of mutex */
                rt_list_remove(&RT_THREAD_LIST_NODE(next_thread));

                /* resume thread to ready queue */
                if (rt_sched_thread_ready(next_thread) != RT_EOK)
                {
                    /**
                     * a timeout timer had triggered while we try. So we skip
                     * this thread and try again.
                     */
                    /* 就绪失败（如恰超时）：跳过该线程，尝试下一个等待者 */
                    next_thread = RT_NULL;
                }
            } while (!next_thread && !rt_list_isempty(&mutex->parent.suspend_thread));

            if (next_thread)
            {
                LOG_D("mutex_release: resume thread: %s",
                    next_thread->parent.name);

                /* set new owner and put mutex into taken list of thread */
                /* 锁直接移交：新 owner 不再经过 take 路径，相当于已持锁一次 */
                mutex->owner = next_thread;
                mutex->hold  = 1;
                rt_list_insert_after(&next_thread->taken_object_list, &mutex->taken_list);

                /* cleanup pending object */
                next_thread->pending_object = RT_NULL;

                /* update mutex priority */
                if (!rt_list_isempty(&(mutex->parent.suspend_thread)))
                {
                    struct rt_thread *th;

                    th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
                    mutex->priority = rt_sched_thread_get_curr_prio(th);
                }
                else
                {
                    mutex->priority = 0xff;
                }

                need_schedule = RT_TRUE;
            }
            else
            {
                /* no waiting thread is woke up, clear owner */
                /* 队列中无人可成功就绪：mutex 变为无 owner */
                mutex->owner = RT_NULL;
                mutex->priority = 0xff;
            }

            rt_sched_unlock(slvl);
        }
        else
        {
            rt_sched_unlock(slvl);

            /* clear owner */
            /* 无阻塞线程：直接释放 */
            mutex->owner    = RT_NULL;
            mutex->priority = 0xff;
        }
    }

    rt_spin_unlock(&(mutex->spinlock));

    /* perform a schedule */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_release);


/**
 * @brief    This function will set some extra attributions of a mutex object.
 *
 * @note     Currently this function does not implement the control function.
 * @note     当前桩实现恒返回 -RT_EINVAL；扩展控制命令需在此处分派。
 *
 * @param    mutex is a pointer to a mutex object.
 *
 * @param    cmd is a command word used to configure some attributions of the mutex.
 *
 * @param    arg is the argument of the function to execute the command.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that this function failed to execute.
 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg)
{
    RT_UNUSED(mutex);
    RT_UNUSED(cmd);
    RT_UNUSED(arg);

    return -RT_EINVAL;
}
RTM_EXPORT(rt_mutex_control);

/**@}*/
#endif /* RT_USING_MUTEX */

/*===========================================================================*/
/* 事件（Event）                                                              */
/*                                                                             */
/* 用 event->set 的位图表示已发生的事件；rt_event_send 对 set 做按位或「置位」。*/
/* 接收方 rt_event_recv 指定关心的 set 及 RT_EVENT_FLAG_AND / OR：           */
/*   AND — (event->set & need) == need 全部位满足才算收到；                    */
/*   OR  — (event->set & need) 非零即满足，并在线程内保存实际触发的位。        */
/* RT_EVENT_FLAG_CLEAR：满足条件时可按规则清除 event->set 中对应位。           */
/* 多线程阻塞顺序由 init/create 的 flag（PRIO/FIFO）决定；并发由 spinlock 保护。*/
/*===========================================================================*/

#ifdef RT_USING_EVENT
/**
 * @addtogroup group_event Event
 * @{
 */

/**
 * @brief    The function will initialize a static event object.
 *
 * @note     For the static event object, its memory space is allocated by the compiler during compiling,
 *           and shall placed on the read-write data segment or on the uninitialized data segment.
 *           By contrast, the rt_event_create() function will allocate memory space automatically
 *           and initialize the event.
 *
 * @see      rt_event_create()
 *
 * @param    event is a pointer to the event to initialize. It is assumed that storage for the event
 *           will be allocated in your application.
 *
 * @param    name is a pointer to the name that given to the event.
 *
 * @param    flag is the event flag, which determines the queuing way of how multiple threads wait
 *           when the event is not available.
 *           The event flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this event will become non-real-time threads.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it represents the initialization failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     静态事件对象；event->set 初值为 0；配对 rt_event_detach。
 */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag)
{
    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* initialize object */
    rt_object_init(&(event->parent.parent), RT_Object_Class_Event, name);

    /* set parent flag */
    event->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(event->parent));

    /* initialize event */
    /* 尚无事件发生，所有位为 0 */
    event->set = 0;
    rt_spin_lock_init(&(event->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_event_init);


/**
 * @brief    This function will detach a static event object.
 *
 * @note     This function is used to detach a static event object which is initialized by rt_event_init() function.
 *           By contrast, the rt_event_delete() function will delete an event object.
 *           When the event is successfully detached, it will resume all suspended threads in the event list.
 *
 * @see      rt_event_delete()
 *
 * @param    event is a pointer to an event object to be detached.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it means that the event detach failed.
 *
 * @warning  This function can ONLY detach a static event initialized by the rt_event_init() function.
 *           If the event is created by the rt_event_create() function, you MUST NOT USE this function to detach it,
 *           ONLY USE the rt_event_delete() function to complete the deletion.
 * @note     唤醒所有阻塞线程并置 RT_ERROR；再从内核容器 detach，不释放静态 event 内存。
 */
rt_err_t rt_event_detach(rt_event_t event)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent));

    level = rt_spin_lock_irqsave(&(event->spinlock));
    /* resume all suspended thread */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    /* detach event object */
    rt_object_detach(&(event->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_event_detach);

#ifdef RT_USING_HEAP
/**
 * @brief    Creating an event object.
 *
 * @note     For the event object, its memory space is allocated automatically.
 *           By contrast, the rt_event_init() function will initialize a static event object.
 *
 * @see      rt_event_init()
 *
 * @param    name is a pointer to the name that given to the event.
 *
 * @param    flag is the event flag, which determines the queuing way of how multiple threads wait when the event
 *           is not available.
 *           The event flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this event will become non-real-time threads.
 *
 * @return   Return a pointer to the event object. When the return value is RT_NULL, it means the creation failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     堆上动态创建，配对 rt_event_delete；不可在 ISR 中调用。
 */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag)
{
    rt_event_t event;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* allocate object */
    event = (rt_event_t)rt_object_allocate(RT_Object_Class_Event, name);
    if (event == RT_NULL)
        return event;

    /* set parent */
    event->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(event->parent));

    /* initialize event */
    event->set = 0;
    rt_spin_lock_init(&(event->spinlock));

    return event;
}
RTM_EXPORT(rt_event_create);


/**
 * @brief    This function will delete an event object and release the memory space.
 *
 * @note     This function is used to delete an event object which is created by the rt_event_create() function.
 *           By contrast, the rt_event_detach() function will detach a static event object.
 *           When the event is successfully deleted, it will resume all suspended threads in the event list.
 *
 * @see      rt_event_detach()
 *
 * @param    event is a pointer to an event object to be deleted.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the event detach failed.
 *
 * @warning  This function can ONLY delete an event initialized by the rt_event_create() function.
 *           If the event is initialized by the rt_event_init() function, you MUST NOT USE this function to delete it,
 *           ONLY USE the rt_event_detach() function to complete the detachment.
 * @note     与 rt_event_create 配对；rt_object_delete 释放控制块及堆资源。
 */
rt_err_t rt_event_delete(rt_event_t event)
{
    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    rt_spin_lock(&(event->spinlock));
    /* resume all suspended thread */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock(&(event->spinlock));

    /* delete event object */
    rt_object_delete(&(event->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_event_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief    This function will send an event to the event object.
 *           If there is a thread suspended on the event, the thread will be resumed.
 *
 * @note     When using this function, you need to use the parameter (set) to specify the event flag of the event object,
 *           then the function will traverse the list of suspended threads waiting on the event object.
 *           If there is a thread suspended on the event, and the thread's event_info and the event flag of
 *           the current event object matches, the thread will be resumed.
 *
 * @param    event is a pointer to the event object to be sent.
 *
 * @param    set is a flag that you will set for this event's flag.
 *           You can set an event flag, or you can set multiple flags through OR logic operation.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the event detach failed.
 * @note     set 非 0；先 event->set |= set 再扫描挂起链。对每个线程用其 recv 时保存的
 *           event_set/event_info（AND/OR/CLEAR）判断是否满足；满足则就绪，并按 CLEAR 累积要清除的位。
 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
    struct rt_list_node *n;
    struct rt_thread *thread;
    rt_sched_lock_level_t slvl;
    rt_base_t level;
    rt_base_t status;
    rt_bool_t need_schedule;
    rt_uint32_t need_clear_set = 0;

    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    if (set == 0)
        return -RT_ERROR;

    need_schedule = RT_FALSE;

    level = rt_spin_lock_irqsave(&(event->spinlock));

    /* set event */
    /* 按位或：可同时通知多个事件位 */
    event->set |= set;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(event->parent.parent)));

    rt_sched_lock(&slvl);
    if (!rt_list_isempty(&event->parent.suspend_thread))
    {
        /* search thread list to resume thread */
        n = event->parent.suspend_thread.next;
        while (n != &(event->parent.suspend_thread))
        {
            /* get thread */
            thread = RT_THREAD_LIST_NODE_ENTRY(n);

            status = -RT_ERROR;
            if (thread->event_info & RT_EVENT_FLAG_AND)
            {
                /* AND：线程等待的 thread->event_set 各位须全部被 event->set 覆盖 */
                if ((thread->event_set & event->set) == thread->event_set)
                {
                    /* received an AND event */
                    status = RT_EOK;
                }
            }
            else if (thread->event_info & RT_EVENT_FLAG_OR)
            {
                /* OR：任一线程关心的位在 event->set 中出现即可 */
                if (thread->event_set & event->set)
                {
                    /* save the received event set */
                    /* 记录实际触发的是哪些位（与上当前全局 set） */
                    thread->event_set = thread->event_set & event->set;

                    /* received an OR event */
                    status = RT_EOK;
                }
            }
            else
            {
                rt_sched_unlock(slvl);
                rt_spin_unlock_irqrestore(&(event->spinlock), level);

                return -RT_EINVAL;
            }

            /* move node to the next */
            n = n->next;

            /* condition is satisfied, resume thread */
            if (status == RT_EOK)
            {
                /* clear event */
                /* 若该线程 recv 时带了 CLEAR，则稍后从 event->set 中清掉其关心的位 */
                if (thread->event_info & RT_EVENT_FLAG_CLEAR)
                    need_clear_set |= thread->event_set;

                /* resume thread, and thread list breaks out */
                rt_sched_thread_ready(thread);
                thread->error = RT_EOK;

                /* need do a scheduling */
                need_schedule = RT_TRUE;
            }
        }
        if (need_clear_set)
        {
            event->set &= ~need_clear_set;
        }
    }

    rt_sched_unlock(slvl);
    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    /* do a schedule */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_event_send);


/**
 * @brief  This function will receive an event from event object. if the event is unavailable, the thread shall wait for
 *         the event up to a specified time.
 *
 * @note   If the current event->set already satisfies the AND/OR condition, the caller returns immediately.
 *         Otherwise the caller may block until rt_event_send() sets matching bits or timeout occurs.
 * @note   中文（实现语义）：若当前 event->set 已满足 AND/OR 条件则立即
 *         返回并在本地清除（若带 CLEAR）；否则 timeout==0 返回 -RT_ETIMEOUT；否则挂起并把本线程的
 *         event_set/event_info 留给 rt_event_send 匹配。被唤醒后 recved 取 thread->event_set（OR 时可能为子集）。
 *
 * @param    event is a pointer to the event object to be received.
 *
 * @param    set is a flag that you will set for this event's flag.
 *           You can set an event flag, or you can set multiple flags through OR logic operation.
 *
 * @param    option is the option of this receiving event, it indicates how the receiving event is operated.
 *           The option can be one or more of the following values, When selecting multiple values,use logical OR to operate.
 *           (NOTE: RT_EVENT_FLAG_OR and RT_EVENT_FLAG_AND can only select one):
 *
 *
 *               RT_EVENT_FLAG_OR           The thread select to use logical OR to receive the event.
 *
 *               RT_EVENT_FLAG_AND          The thread select to use logical OR to receive the event.
 *
 *               RT_EVENT_FLAG_CLEAR        When the thread receives the corresponding event, the function
 *                                          determines whether to clear the event flag.
 *
 * @param    timeout is a timeout period (unit: an OS tick).
 *
 * @param    recved is a pointer to the received event. If you don't care about this value, you can use RT_NULL to set.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the event receive failed.
 */
static rt_err_t _rt_event_recv(rt_event_t   event,
                               rt_uint32_t  set,
                               rt_uint8_t   option,
                               rt_int32_t   timeout,
                               rt_uint32_t *recved,
                               int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_base_t status;
    rt_err_t ret;

    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    if (set == 0)
        return -RT_ERROR;

    /* initialize status */
    status = -RT_ERROR;
    /* get current thread */
    thread = rt_thread_self();
    /* reset thread error */
    thread->error = -RT_EINTR;

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(event->parent.parent)));

    level = rt_spin_lock_irqsave(&(event->spinlock));

    /* check event set */
    if (option & RT_EVENT_FLAG_AND)
    {
        /* AND：set 中各位须已全部出现在 event->set 中 */
        if ((event->set & set) == set)
            status = RT_EOK;
    }
    else if (option & RT_EVENT_FLAG_OR)
    {
        /* OR：set 中任一位已被置位即可 */
        if (event->set & set)
            status = RT_EOK;
    }
    else
    {
        /* either RT_EVENT_FLAG_AND or RT_EVENT_FLAG_OR should be set */
        RT_ASSERT(0);
    }

    if (status == RT_EOK)
    {
        thread->error = RT_EOK;

        /* set received event */
        if (recved)
            *recved = (event->set & set);

        /* fill thread event info */
        /* 与 rt_event_send 中唤醒路径使用的字段一致 */
        thread->event_set = (event->set & set);
        thread->event_info = option;

        /* received event */
        /* 接收端立即清除全局标志中本次关心的位（与 send 路径 CLEAR 语义配合） */
        if (option & RT_EVENT_FLAG_CLEAR)
            event->set &= ~set;
    }
    else if (timeout == 0)
    {
        /* no waiting */
        thread->error = -RT_ETIMEOUT;

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        return -RT_ETIMEOUT;
    }
    else
    {
        /* fill thread event info */
        /* 记录本线程等待的位掩码与 AND/OR/CLEAR，供 send 扫描 */
        thread->event_set  = set;
        thread->event_info = option;

        /* put thread to suspended thread list */
        ret = rt_thread_suspend_to_list(thread, &(event->parent.suspend_thread),
                                        event->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(event->spinlock), level);
            return ret;
        }

        /* if there is a waiting timeout, active thread timer */
        if (timeout > 0)
        {
            /* reset the timeout of thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        /* do a schedule */
        rt_schedule();

        if (thread->error != RT_EOK)
        {
            /* return error */
            return thread->error;
        }

        /* received an event, disable interrupt to protect */
        level = rt_spin_lock_irqsave(&(event->spinlock));

        /* set received event */
        /* OR 唤醒时 thread->event_set 可能已为实际触发的子集 */
        if (recved)
            *recved = thread->event_set;
    }

    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(event->parent.parent)));

    return thread->error;
}

/* 默认阻塞 recv：挂起期间不可被信号打断（RT_UNINTERRUPTIBLE） */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv);

rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv_interruptible);

rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_KILLABLE);
}
RTM_EXPORT(rt_event_recv_killable);
/**
 * @brief    This function will set some extra attributions of an event object.
 *
 * @note     Currently this function only supports the RT_IPC_CMD_RESET command to reset the event.
 * @note     RESET 时唤醒所有等待线程（RT_ERROR）、event->set 置 0，并 rt_schedule。
 *
 * @param    event is a pointer to an event object.
 *
 * @param    cmd is a command word used to configure some attributions of the event.
 *
 * @param    arg is the argument of the function to execute the command.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that this function failed to execute.
 */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg)
{
    rt_base_t level;

    RT_UNUSED(arg);

    /* parameter check */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(event->spinlock));

        /* resume all waiting thread */
        rt_susp_list_resume_all(&event->parent.suspend_thread, RT_ERROR);

        /* initialize event set */
        /* 清除所有已记录事件位 */
        event->set = 0;

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    /* 其它 cmd 未实现 */
    return -RT_ERROR;
}
RTM_EXPORT(rt_event_control);

/**@}*/
#endif /* RT_USING_EVENT */

/*===========================================================================*/
/* 邮箱（MailBox）                                                            */
/*                                                                             */
/* 定长环形缓冲 msg_pool[]，元素类型 rt_ubase_t（通常可存指针或整数）。        */
/* entry 为当前邮件数；in_offset / out_offset 为环形队尾入、队头出下标。       */
/* 接收阻塞挂在 parent.suspend_thread；邮箱满时发送阻塞挂在 suspend_sender_thread。*/
/* rt_mb_send 为 timeout=0 的 rt_mb_send_wait；rt_mb_urgent 将邮件插到队头侧。 */
/*===========================================================================*/

#ifdef RT_USING_MAILBOX
/**
 * @addtogroup group_mailbox MailBox
 * @{
 */

/**
 * @brief    Initialize a static mailbox object.
 *
 * @note     For the static mailbox object, its memory space is allocated by the compiler during compiling,
 *           and shall placed on the read-write data segment or on the uninitialized data segment.
 *           By contrast, the rt_mb_create() function will allocate memory space automatically and initialize the mailbox.
 *
 * @see      rt_mb_create()
 *
 * @param    mb is a pointer to the mailbox to initialize.
 *           It is assumed that storage for the mailbox will be allocated in your application.
 *
 * @param    name is a pointer to the name that given to the mailbox.
 *
 * @param    msgpool the begin address of buffer to save received mail.
 *
 * @param    size is the maximum number of mails in the mailbox.
 *           For example, when the mailbox buffer capacity is N, size is N/4.
 *
 * @param    flag is the mailbox flag, which determines the queuing way of how multiple threads wait
 *           when the mailbox is not available.
 *           The mailbox flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                       (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this mailbox will become non-real-time threads.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it represents the initialization failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     静态邮箱；msgpool 指向用户提供的缓冲区，长度至少 size * sizeof(rt_ubase_t)；配对 rt_mb_detach。
 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag)
{
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* initialize object */
    rt_object_init(&(mb->parent.parent), RT_Object_Class_MailBox, name);

    /* set parent flag */
    mb->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(mb->parent));

    /* initialize mailbox */
    /* msgpool：环形队列存储区；size 为槽位数（可容纳邮件个数） */
    mb->msg_pool   = (rt_ubase_t *)msgpool;
    mb->size       = (rt_uint16_t)size;
    mb->entry      = 0;
    mb->in_offset  = 0;
    mb->out_offset = 0;

    /* initialize an additional list of sender suspend thread */
    /* 邮箱满时阻塞的发送者挂在此链，与接收者挂起的 parent.suspend_thread 分离 */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_init);


/**
 * @brief    This function will detach a static mailbox object.
 *
 * @note     This function is used to detach a static mailbox object which is initialized by rt_mb_init() function.
 *           By contrast, the rt_mb_delete() function will delete a mailbox object.
 *           When the mailbox is successfully detached, it will resume all suspended threads in the mailbox list.
 *
 * @see      rt_mb_delete()
 *
 * @param    mb is a pointer to a mailbox object to be detached.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it means that the mailbox detach failed.
 *
 * @warning  This function can ONLY detach a static mailbox initialized by the rt_mb_init() function.
 *           If the mailbox is created by the rt_mb_create() function, you MUST NOT USE this function to detach it,
 *           ONLY USE the rt_mb_delete() function to complete the deletion.
 * @note     同时唤醒接收者与满邮箱上阻塞的发送者；不释放用户提供的 msgpool。
 */
rt_err_t rt_mb_detach(rt_mailbox_t mb)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent));

    level = rt_spin_lock_irqsave(&(mb->spinlock));
    /* resume all suspended thread */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
    /* also resume all mailbox private suspended thread */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    /* detach mailbox object */
    rt_object_detach(&(mb->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_detach);

#ifdef RT_USING_HEAP
/**
 * @brief  Creating a mailbox object.
 *
 * @note   For the mailbox object, its memory space is allocated automatically.
 *         By contrast, the rt_mb_init() function will initialize a static mailbox object.
 *
 * @see    rt_mb_init()
 *
 * @param  name is a pointer that given to the mailbox.
 *
 * @param    size is the maximum number of mails in the mailbox.
 *           For example, when mailbox buffer capacity is N, size is N/4.
 *
 * @param    flag is the mailbox flag, which determines the queuing way of how multiple threads wait
 *           when the mailbox is not available.
 *           The mailbox flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this mailbox will become non-real-time threads.
 *
 * @return   Return a pointer to the mailbox object. When the return value is RT_NULL, it means the creation failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     堆上分配控制块与 msg_pool（size 个 rt_ubase_t）；配对 rt_mb_delete。
 */
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag)
{
    rt_mailbox_t mb;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* allocate object */
    mb = (rt_mailbox_t)rt_object_allocate(RT_Object_Class_MailBox, name);
    if (mb == RT_NULL)
        return mb;

    /* set parent */
    mb->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(mb->parent));

    /* initialize mailbox */
    mb->size     = (rt_uint16_t)size;
    mb->msg_pool = (rt_ubase_t *)RT_KERNEL_MALLOC(mb->size * sizeof(rt_ubase_t));
    if (mb->msg_pool == RT_NULL)
    {
        /* delete mailbox object */
        rt_object_delete(&(mb->parent.parent));

        return RT_NULL;
    }
    mb->entry      = 0;
    mb->in_offset  = 0;
    mb->out_offset = 0;

    /* initialize an additional list of sender suspend thread */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock));

    return mb;
}
RTM_EXPORT(rt_mb_create);


/**
 * @brief    This function will delete a mailbox object and release the memory space.
 *
 * @note     This function is used to delete a mailbox object which is created by the rt_mb_create() function.
 *           By contrast, the rt_mb_detach() function will detach a static mailbox object.
 *           When the mailbox is successfully deleted, it will resume all suspended threads in the mailbox list.
 *
 * @see      rt_mb_detach()
 *
 * @param    mb is a pointer to a mailbox object to be deleted.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mailbox detach failed.
 *
 * @warning  This function can only delete mailbox created by the rt_mb_create() function.
 *           If the mailbox is initialized by the rt_mb_init() function, you MUST NOT USE this function to delete it,
 *           ONLY USE the rt_mb_detach() function to complete the detachment.
 * @note     与 rt_mb_create 配对；先唤醒两类阻塞线程，再释放 msg_pool 与对象内存。
 */
rt_err_t rt_mb_delete(rt_mailbox_t mb)
{
    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;
    rt_spin_lock(&(mb->spinlock));

    /* resume all suspended thread */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);

    /* also resume all mailbox private suspended thread */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mb->spinlock));

    /* free mailbox pool */
    /* create 时内部分配的环形缓冲 */
    RT_KERNEL_FREE(mb->msg_pool);

    /* delete mailbox object */
    rt_object_delete(&(mb->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief    This function will send an mail to the mailbox object. If there is a thread suspended on the mailbox,
 *           the thread will be resumed.
 *
 * @note     When using this function to send a mail, if the mailbox if fully used, the current thread will
 *           wait for a timeout. If the set timeout time is reached and there is still no space available,
 *           the sending thread will be resumed and an error code will be returned.
 *           By contrast, the rt_mb_send() function will return an error code immediately without waiting time
 *           when the mailbox if fully used.
 *
 * @see      rt_mb_send()
 *
 * @param    mb is a pointer to the mailbox object to be sent.
 *
 * @param    value is a value to the content of the mail you want to send.
 *
 * @param    timeout is a timeout period (unit: an OS tick).
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mailbox detach failed.
 *
 * @warning  This function can be called in interrupt context and thread context.
 * @note     entry==size 为满；满时若 timeout==0 返回 -RT_EFULL；否则挂到 suspend_sender_thread。
 *           timeout>0 时在循环中扣减已消耗 tick 以支持累计限时。非阻塞发送可在 ISR（timeout==0）。
 */
static rt_err_t _rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout,
                         int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_uint32_t tick_delta;
    rt_err_t ret;

    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* initialize delta tick */
    tick_delta = 0;
    /* get current thread */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent)));

    /* disable interrupt */
    level = rt_spin_lock_irqsave(&(mb->spinlock));

    /* for non-blocking call */
    if (mb->entry == mb->size && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL;
    }

    /* mailbox is full */
    /* 满则阻塞发送线程，直至 recv 腾出空位或超时 */
    while (mb->entry == mb->size)
    {
        /* reset error number in thread */
        thread->error = -RT_EINTR;

        /* no waiting, return timeout */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);

            return -RT_EFULL;
        }

        /* suspend current thread */
        ret = rt_thread_suspend_to_list(thread, &(mb->suspend_sender_thread),
                                        mb->parent.parent.flag, suspend_flag);

        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);
            return ret;
        }

        /* has waiting time, start thread timer */
        if (timeout > 0)
        {
            /* get the start tick of timer */
            tick_delta = rt_tick_get();

            LOG_D("mb_send_wait: start timer of thread:%s",
                  thread->parent.name);

            /* reset the timeout of thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout);
            rt_timer_start(&(thread->thread_timer));
        }
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        /* re-schedule */
        rt_schedule();

        /* resume from suspend state */
        if (thread->error != RT_EOK)
        {
            /* return error */
            return thread->error;
        }

        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* if it's not waiting forever and then re-calculate timeout tick */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* set ptr */
    /* 环形队列队尾写入 */
    mb->msg_pool[mb->in_offset] = value;
    /* increase input offset */
    ++ mb->in_offset;
    if (mb->in_offset >= mb->size)
        mb->in_offset = 0;

    if(mb->entry < RT_MB_ENTRY_MAX)
    {
        /* increase message entry */
        mb->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL; /* value overflowed */
    }

    /* resume suspended thread */
    /* 若有线程因邮箱空而阻塞在 recv，唤醒队首一个 */
    if (!rt_list_isempty(&mb->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    return RT_EOK;
}

/* 默认可阻塞发送；suspend_flag 控制挂起是否可被信号打断 */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait);

rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait_interruptible);

rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_send_wait_killable);
/**
 * @brief    This function will send an mail to the mailbox object. If there is a thread suspended on the mailbox,
 *           the thread will be resumed.
 *
 * @note     When using this function to send a mail, if the mailbox is fully used, this function will return an error
 *           code immediately without waiting time.
 *           By contrast, the rt_mb_send_wait() function is set a timeout to wait for the mail to be sent.
 *
 * @see      rt_mb_send_wait()
 *
 * @param    mb is a pointer to the mailbox object to be sent.
 *
 * @param    value is a value to the content of the mail you want to send.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mailbox detach failed.
 * @note     即 rt_mb_send_wait(mb, value, 0)；满则立即 -RT_EFULL，不阻塞。
 */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait(mb, value, 0);
}
RTM_EXPORT(rt_mb_send);

/* 非阻塞发送 + 可中断挂起语义包装（timeout 仍为 0） */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_interruptible(mb, value, 0);
}
RTM_EXPORT(rt_mb_send_interruptible);

rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_killable(mb, value, 0);
}
RTM_EXPORT(rt_mb_send_killable);

/**
 * @brief    This function will send an urgent mail to the mailbox object.
 *
 * @note     This function is almost the same as the rt_mb_send() function. The only difference is that
 *           when sending an urgent mail, the mail will be placed at the head of the mail queue so that
 *           the recipient can receive the urgent mail first.
 *
 * @see      rt_mb_send()
 *
 * @param    mb is a pointer to the mailbox object to be sent.
 *
 * @param    value is the content of the mail you want to send.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mailbox detach failed.
 * @note     不阻塞；通过回绕 out_offset 把邮件插在「队头」一侧，recv 仍按 out 顺序会先取到该 urgent。
 */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent)));

    level = rt_spin_lock_irqsave(&(mb->spinlock));

    if (mb->entry == mb->size)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL;
    }

    /* rewind to the previous position */
    /* 逻辑上在队头插入：令 out 回退一格再写入该槽 */
    if (mb->out_offset > 0)
    {
        mb->out_offset --;
    }
    else
    {
        mb->out_offset = mb->size - 1;
    }

    /* set ptr */
    mb->msg_pool[mb->out_offset] = value;

    /* increase message entry */
    mb->entry ++;

    /* resume suspended thread */
    if (!rt_list_isempty(&mb->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mb_urgent);


/**
 * @brief    This function will receive a mail from mailbox object, if there is no mail in mailbox object,
 *           the thread shall wait for a specified time.
 *
 * @note     Only when there is mail in the mailbox, the receiving thread can get the mail immediately and
 *           return RT_EOK, otherwise the receiving thread will be suspended until the set timeout. If the mail
 *           is still not received within the specified time, it will return-RT_ETIMEOUT.
 *
 * @param    mb is a pointer to the mailbox object to be received.
 *
 * @param    value is a flag that you will set for this mailbox's flag.
 *           You can set an mailbox flag, or you can set multiple flags through OR logic operations.
 *
 * @param    timeout is a timeout period (unit: an OS tick). If the mailbox object is not avaliable in the queue,
 *           the thread will wait for the object in the queue up to the amount of time specified by this parameter.
 *
 *           NOTE:
 *           If use Macro RT_WAITING_FOREVER to set this parameter, which means that when the
 *           mailbox object is unavailable in the queue, the thread will be waiting forever.
 *           If use macro RT_WAITING_NO to set this parameter, which means that this
 *           function is non-blocking and will return immediately.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the mailbox receive failed.
 * @note     value 为出参指针，用于接收一条 rt_ubase_t 邮件。entry==0 为空；空且 timeout==0 返回
 *           -RT_ETIMEOUT。取信后若有发送者在 suspend_sender_thread 上阻塞，唤醒其一。
 */
static rt_err_t _rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_uint32_t tick_delta;
    rt_err_t ret;

    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* initialize delta tick */
    tick_delta = 0;
    /* get current thread */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mb->parent.parent)));

    level = rt_spin_lock_irqsave(&(mb->spinlock));

    /* for non-blocking call */
    if (mb->entry == 0 && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        return -RT_ETIMEOUT;
    }

    /* mailbox is empty */
    /* 空则阻塞在 parent.suspend_thread，直至 send/urgent 投递或超时 */
    while (mb->entry == 0)
    {
        /* reset error number in thread */
        thread->error = -RT_EINTR;

        /* no waiting, return timeout */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);

            thread->error = -RT_ETIMEOUT;

            return -RT_ETIMEOUT;
        }

        /* suspend current thread */
        ret = rt_thread_suspend_to_list(thread, &(mb->parent.suspend_thread),
                                        mb->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);
            return ret;
        }

        /* has waiting time, start thread timer */
        if (timeout > 0)
        {
            /* get the start tick of timer */
            tick_delta = rt_tick_get();

            LOG_D("mb_recv: start timer of thread:%s",
                  thread->parent.name);

            /* reset the timeout of thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        /* re-schedule */
        rt_schedule();

        /* resume from suspend state */
        if (thread->error != RT_EOK)
        {
            /* return error */
            return thread->error;
        }
        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* if it's not waiting forever and then re-calculate timeout tick */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* fill ptr */
    /* 从队头 out_offset 取出一封邮件 */
    *value = mb->msg_pool[mb->out_offset];

    /* increase output offset */
    ++ mb->out_offset;
    if (mb->out_offset >= mb->size)
        mb->out_offset = 0;

    /* decrease message entry */
    if(mb->entry > 0)
    {
        mb->entry --;
    }

    /* resume suspended thread */
    /* 腾出空位后若仍有发送者因满而阻塞，唤醒队首 */
    if (!rt_list_isempty(&(mb->suspend_sender_thread)))
    {
        rt_susp_list_dequeue(&(mb->suspend_sender_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent)));

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent)));

    return RT_EOK;
}

/* 默认 recv：阻塞等待不可被信号打断 */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv);

rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv_interruptible);

rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_recv_killable);

/**
 * @brief    This function will set some extra attributions of a mailbox object.
 *
 * @note     Currently this function only supports the RT_IPC_CMD_RESET command to reset the mailbox.
 * @note     RESET 唤醒所有 recv/send 阻塞线程（RT_ERROR），清空 entry 与环形下标（不 memset 池内容，
 *           但逻辑上邮箱为空）。
 *
 * @param    mb is a pointer to a mailbox object.
 *
 * @param    cmd is a command used to configure some attributions of the mailbox.
 *
 * @param    arg is the argument of the function to execute the command.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that this function failed to execute.
 */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg)
{
    rt_base_t level;

    RT_UNUSED(arg);

    /* parameter check */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* resume all waiting thread */
        rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
        /* also resume all mailbox private suspended thread */
        rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

        /* re-init mailbox */
        /* 丢弃未读邮件计数，池内旧数据不再通过合法下标访问 */
        mb->entry      = 0;
        mb->in_offset  = 0;
        mb->out_offset = 0;

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    /* 其它 cmd 未实现 */
    return -RT_ERROR;
}
RTM_EXPORT(rt_mb_control);

/**@}*/
#endif /* RT_USING_MAILBOX */

/*===========================================================================*/
/* 消息队列（Message Queue）                                                  */
/*                                                                             */
/* 内存池划分为 max_msgs 个「消息头 rt_mq_message + 对齐后的载荷」槽位；       */
/* msg_queue_free 为空闲链表，msg_queue_head/tail 为已投递消息的 FIFO 链表     */
/*（RT_USING_MESSAGEQUEUE_PRIORITY 时按 msg->prio 高优先级在前插入）。         */
/* entry 为当前队列中消息条数；发送满时阻塞在 suspend_sender_thread，          */
/* 接收空时阻塞在 parent.suspend_thread（与邮箱对称）。                        */
/*===========================================================================*/

#ifdef RT_USING_MESSAGEQUEUE
/**
 * @addtogroup group_messagequeue Message Queue
 * @{
 */

/**
 * @brief    Initialize a static messagequeue object.
 *
 * @note     For the static messagequeue object, its memory space is allocated by the compiler during compiling,
 *           and shall placed on the read-write data segment or on the uninitialized data segment.
 *           By contrast, the rt_mq_create() function will allocate memory space automatically
 *           and initialize the messagequeue.
 *
 * @see      rt_mq_create()
 *
 * @param    mq is a pointer to the messagequeue to initialize. It is assumed that storage for
 *           the messagequeue will be allocated in your application.
 *
 * @param    name is a pointer to the name that given to the messagequeue.
 *
 * @param    msgpool is a pointer to the starting address of the memory space you allocated for
 *           the messagequeue in advance.
 *           In other words, msgpool is a pointer to the messagequeue buffer of the starting address.
 *
 * @param    msg_size is the maximum length of a message in the messagequeue (Unit: Byte).
 *
 * @param    pool_size is the size of the memory space allocated for the messagequeue in advance.
 *
 * @param    flag is the messagequeue flag, which determines the queuing way of how multiple threads wait
 *           when the messagequeue is not available.
 *           The messagequeue flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this messagequeue will become non-real-time threads.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it represents the initialization failed.
 *
 * @warning  This function can ONLY be called from threads.
 * @note     静态消息队列；msgpool 为连续内存，pool_size 为字节总长；max_msgs 由槽大小反推。
 *           单条消息最大字节数为 msg_size（发送时不可超过）。配对 rt_mq_detach。
 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag)
{
    struct rt_mq_message *head;
    rt_base_t temp;
    register rt_size_t msg_align_size;

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* initialize object */
    rt_object_init(&(mq->parent.parent), RT_Object_Class_MessageQueue, name);

    /* set parent flag */
    mq->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(mq->parent));

    /* set message pool */
    mq->msg_pool = msgpool;

    /* get correct message size */
    /* 每条消息占用：头 sizeof(rt_mq_message) + 对齐后的用户数据区 msg_align_size */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE);
    mq->msg_size = msg_size;
    mq->max_msgs = pool_size / (msg_align_size + sizeof(struct rt_mq_message));

    if (0 == mq->max_msgs)
    {
        return -RT_EINVAL;
    }

    /* initialize message list */
    mq->msg_queue_head = RT_NULL;
    mq->msg_queue_tail = RT_NULL;

    /* initialize message empty list */
    /* 将所有槽通过 next 串成空闲栈（头插法），后续发送从 msg_queue_free 取头结点 */
    mq->msg_queue_free = RT_NULL;
    for (temp = 0; temp < mq->max_msgs; temp ++)
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message)));
        head->next = (struct rt_mq_message *)mq->msg_queue_free;
        mq->msg_queue_free = head;
    }

    /* the initial entry is zero */
    mq->entry = 0;

    /* initialize an additional list of sender suspend thread */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_init);


/**
 * @brief    This function will detach a static messagequeue object.
 *
 * @note     This function is used to detach a static messagequeue object which is initialized by rt_mq_init() function.
 *           By contrast, the rt_mq_delete() function will delete a messagequeue object.
 *           When the messagequeue is successfully detached, it will resume all suspended threads in the messagequeue list.
 *
 * @see      rt_mq_delete()
 *
 * @param    mq is a pointer to a messagequeue object to be detached.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the initialization is successful.
 *           If the return value is any other values, it means that the messagequeue detach failed.
 *
 * @warning  This function can ONLY detach a static messagequeue initialized by the rt_mq_init() function.
 *           If the messagequeue is created by the rt_mq_create() function, you MUST NOT USE this function to detach it,
 *           and ONLY USE the rt_mq_delete() function to complete the deletion.
 * @note     唤醒 recv/send 两侧阻塞线程；不释放用户提供的 msgpool。
 */
rt_err_t rt_mq_detach(rt_mq_t mq)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent));

    level = rt_spin_lock_irqsave(&(mq->spinlock));
    /* resume all suspended thread */
    rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
    /* also resume all message queue private suspended thread */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* detach message queue object */
    rt_object_detach(&(mq->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_detach);

#ifdef RT_USING_HEAP
/**
 * @brief    Creating a messagequeue object.
 *
 * @note     For the messagequeue object, its memory space is allocated automatically.
 *           By contrast, the rt_mq_init() function will initialize a static messagequeue object.
 *
 * @see      rt_mq_init()
 *
 * @param    name is a pointer that given to the messagequeue.
 *
 * @param    msg_size is the maximum length of a message in the messagequeue (Unit: Byte).
 *
 * @param    max_msgs is the maximum number of messages in the messagequeue.
 *
 * @param    flag is the messagequeue flag, which determines the queuing way of how multiple threads wait
 *           when the messagequeue is not available.
 *           The messagequeue flag can be ONE of the following values:
 *
 *               RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *
 *               RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 *                                         (also known as first-come-first-served (FCFS) scheduling strategy).
 *
 *               NOTE: RT_IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly recommended to
 *               use RT_IPC_FLAG_PRIO to ensure the thread is real-time UNLESS your applications concern about
 *               the first-in-first-out principle, and you clearly understand that all threads involved in
 *               this messagequeue will become non-real-time threads.
 *
 * @return   Return a pointer to the messagequeue object. When the return value is RT_NULL, it means the creation failed.
 *
 * @warning  This function can NOT be called in interrupt context. You can use macor RT_DEBUG_NOT_IN_INTERRUPT to check it.
 * @note     堆上分配控制块与 msg_pool；max_msgs 为槽个数。配对 rt_mq_delete。
 */
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag)
{
    struct rt_messagequeue *mq;
    struct rt_mq_message *head;
    rt_base_t temp;
    register rt_size_t msg_align_size;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* allocate object */
    mq = (rt_mq_t)rt_object_allocate(RT_Object_Class_MessageQueue, name);
    if (mq == RT_NULL)
        return mq;

    /* set parent */
    mq->parent.parent.flag = flag;

    /* initialize ipc object */
    _ipc_object_init(&(mq->parent));

    /* initialize message queue */

    /* get correct message size */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE);
    mq->msg_size = msg_size;
    mq->max_msgs = max_msgs;

    /* allocate message pool */
    mq->msg_pool = RT_KERNEL_MALLOC((msg_align_size + sizeof(struct rt_mq_message)) * mq->max_msgs);
    if (mq->msg_pool == RT_NULL)
    {
        rt_object_delete(&(mq->parent.parent));

        return RT_NULL;
    }

    /* initialize message list */
    mq->msg_queue_head = RT_NULL;
    mq->msg_queue_tail = RT_NULL;

    /* initialize message empty list */
    mq->msg_queue_free = RT_NULL;
    for (temp = 0; temp < mq->max_msgs; temp ++)
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message)));
        head->next = (struct rt_mq_message *)mq->msg_queue_free;
        mq->msg_queue_free = head;
    }

    /* the initial entry is zero */
    mq->entry = 0;

    /* initialize an additional list of sender suspend thread */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock));

    return mq;
}
RTM_EXPORT(rt_mq_create);


/**
 * @brief    This function will delete a messagequeue object and release the memory.
 *
 * @note     This function is used to delete a messagequeue object which is created by the rt_mq_create() function.
 *           By contrast, the rt_mq_detach() function will detach a static messagequeue object.
 *           When the messagequeue is successfully deleted, it will resume all suspended threads in the messagequeue list.
 *
 * @see      rt_mq_detach()
 *
 * @param    mq is a pointer to a messagequeue object to be deleted.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the messagequeue detach failed.
 *
 * @warning  This function can ONLY delete a messagequeue initialized by the rt_mq_create() function.
 *           If the messagequeue is initialized by the rt_mq_init() function, you MUST NOT USE this function to delete it,
 *           ONLY USE the rt_mq_detach() function to complete the detachment.
 *           for example,the rt_mq_create() function, it cannot be called in interrupt context.
 * @note     与 rt_mq_create 配对；先唤醒 recv/send 阻塞线程，再 RT_KERNEL_FREE(msg_pool) 并删除对象。
 */
rt_err_t rt_mq_delete(rt_mq_t mq)
{
    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    rt_spin_lock(&(mq->spinlock));
    /* resume all suspended thread */
    rt_susp_list_resume_all(&(mq->parent.suspend_thread), RT_ERROR);
    /* also resume all message queue private suspended thread */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mq->spinlock));

    /* free message queue pool */
    /* create 时一次性分配整块池 */
    RT_KERNEL_FREE(mq->msg_pool);

    /* delete message queue object */
    rt_object_delete(&(mq->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief    This function will send a message to the messagequeue object. If
 *           there is a thread suspended on the messagequeue, the thread will be
 *           resumed.
 *
 * @note     When using this function to send a message, if the messagequeue is
 *           fully used, the current thread will wait for a timeout. If reaching
 *           the timeout and there is still no space available, the sending
 *           thread will be resumed and an error code will be returned. By
 *           contrast, the _rt_mq_send_wait() function will return an error code
 *           immediately without waiting when the messagequeue if fully used.
 *
 * @see      _rt_mq_send_wait()
 *
 * @param    mq is a pointer to the messagequeue object to be sent.
 *
 * @param    buffer is the content of the message.
 *
 * @param    size is the length of the message(Unit: Byte).
 *
 * @param    prio is message priority, A larger value indicates a higher priority
 *
 * @param    timeout is a timeout period (unit: an OS tick).
 *
 * @param    suspend_flag status flag of the thread to be suspended.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the
 *           operation is successful. If the return value is any other values,
 *           it means that the messagequeue detach failed.
 *
 * @warning  This function can be called in interrupt context and thread
 * context.
 * @note     从 msg_queue_free 取空闲槽；无槽且 timeout==0 返回 -RT_EFULL；否则在 suspend_sender_thread
 *           上阻塞。拷贝 buffer 到 GET_MESSAGEBYTE_ADDR(msg)；无优先级特性时 prio 被 RT_UNUSED 忽略。
 */
static rt_err_t _rt_mq_send_wait(rt_mq_t mq,
                                 const void *buffer,
                                 rt_size_t size,
                                 rt_int32_t prio,
                                 rt_int32_t timeout,
                                 int suspend_flag)
{
    rt_base_t level;
    struct rt_mq_message *msg;
    rt_uint32_t tick_delta;
    struct rt_thread *thread;
    rt_err_t ret;

    RT_UNUSED(prio);

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* greater than one message size */
    if (size > mq->msg_size)
        return -RT_ERROR;

    /* initialize delta tick */
    tick_delta = 0;
    /* get current thread */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* get a free list, there must be an empty item */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* for non-blocking call */
    if (msg == RT_NULL && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_EFULL;
    }

    /* message queue is full */
    /* 无空闲槽则与邮箱满时类似：发送线程挂起直至 recv 归还槽位 */
    while ((msg = (struct rt_mq_message *)mq->msg_queue_free) == RT_NULL)
    {
        /* reset error number in thread */
        thread->error = -RT_EINTR;

        /* no waiting, return timeout */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);

            return -RT_EFULL;
        }

        /* suspend current thread */
        ret = rt_thread_suspend_to_list(thread, &(mq->suspend_sender_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);
            return ret;
        }

        /* has waiting time, start thread timer */
        if (timeout > 0)
        {
            /* get the start tick of timer */
            tick_delta = rt_tick_get();

            LOG_D("mq_send_wait: start timer of thread:%s",
                  thread->parent.name);

            /* reset the timeout of thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        /* re-schedule */
        rt_schedule();

        /* resume from suspend state */
        if (thread->error != RT_EOK)
        {
            /* return error */
            return thread->error;
        }
        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* if it's not waiting forever and then re-calculate timeout tick */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* move free list pointer */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* the msg is the new tailer of list, the next shall be NULL */
    msg->next = RT_NULL;

    /* add the length */
    ((struct rt_mq_message *)msg)->length = size;
    /* copy buffer */
    /* 载荷紧跟在 rt_mq_message 头之后（见 GET_MESSAGEBYTE_ADDR） */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    /* disable interrupt */
    level = rt_spin_lock_irqsave(&(mq->spinlock));
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    /* 高 prio 数值更大：插入有序链表使高优先级消息更靠近队头先被 recv */
    msg->prio = prio;
    if (mq->msg_queue_head == RT_NULL)
        mq->msg_queue_head = msg;

    struct rt_mq_message *node, *prev_node = RT_NULL;
    for (node = mq->msg_queue_head; node != RT_NULL; node = node->next)
    {
        if (node->prio < msg->prio)
        {
            if (prev_node == RT_NULL)
                mq->msg_queue_head = msg;
            else
                prev_node->next = msg;
            msg->next = node;
            break;
        }
        if (node->next == RT_NULL)
        {
            if (node != msg)
                node->next = msg;
            mq->msg_queue_tail = msg;
            break;
        }
        prev_node = node;
    }
#else
    /* link msg to message queue */
    /* 标准 FIFO：尾插法 */
    if (mq->msg_queue_tail != RT_NULL)
    {
        /* if the tail exists, */
        ((struct rt_mq_message *)mq->msg_queue_tail)->next = msg;
    }

    /* set new tail */
    mq->msg_queue_tail = msg;
    /* if the head is empty, set head */
    if (mq->msg_queue_head == RT_NULL)
        mq->msg_queue_head = msg;
#endif

    if(mq->entry < RT_MQ_ENTRY_MAX)
    {
        /* increase message entry */
        mq->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);
        return -RT_EFULL; /* value overflowed */
    }

    /* resume suspended thread */
    if (!rt_list_isempty(&mq->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    return RT_EOK;
}

/* prio 传 0；使能 MESSAGEQUEUE_PRIORITY 时请用 rt_mq_send_wait_prio */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait);

rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait_interruptible);

rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mq_send_wait_killable);
/**
 * @brief    This function will send a message to the messagequeue object.
 *           If there is a thread suspended on the messagequeue, the thread will be resumed.
 *
 * @note     This function is equivalent to rt_mq_send_wait() with timeout 0 (non-blocking).
 *           If the queue is full, it returns -RT_EFULL immediately. Use rt_mq_send_wait() to block.
 *
 * @see      rt_mq_send_wait()
 *
 * @param    mq is a pointer to the messagequeue object to be sent.
 *
 * @param    buffer is the content of the message.
 *
 * @param    size is the length of the message(Unit: Byte).
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the message queue send failed.
 *
 * @warning  This function can be called in interrupt context and thread context.
 * @note     即 rt_mq_send_wait(..., timeout=0)；队列满立即 -RT_EFULL。
 */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send);

rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_interruptible(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send_interruptible);

rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_killable(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send_killable);
/**
 * @brief    This function will send an urgent message to the messagequeue object.
 *
 * @note     This function is almost the same as the rt_mq_send() function. The only difference is that
 *           when sending an urgent message, the message is placed at the head of the messagequeue so that
 *           the recipient can receive the urgent message first.
 *
 * @see      rt_mq_send()
 *
 * @param    mq is a pointer to the messagequeue object to be sent.
 *
 * @param    buffer is the content of the message.
 *
 * @param    size is the length of the message(Unit: Byte).
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that the message queue send failed.
 * @note     非阻塞；无空闲槽返回 -RT_EFULL；否则头插 msg_queue_head，recv 优先取到。
 */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    rt_base_t level;
    struct rt_mq_message *msg;

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* greater than one message size */
    if (size > mq->msg_size)
        return -RT_ERROR;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* get a free list, there must be an empty item */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* message queue is full */
    if (msg == RT_NULL)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_EFULL;
    }
    /* move free list pointer */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* add the length */
    ((struct rt_mq_message *)msg)->length = size;
    /* copy buffer */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* link msg to the beginning of message queue */
    /* 紧急报文插到队头（不调整优先级字段，与 PRIO 编译选项独立） */
    msg->next = (struct rt_mq_message *)mq->msg_queue_head;
    mq->msg_queue_head = msg;

    /* if there is no tail */
    if (mq->msg_queue_tail == RT_NULL)
        mq->msg_queue_tail = msg;

    if(mq->entry < RT_MQ_ENTRY_MAX)
    {
        /* increase message entry */
        mq->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);
        return -RT_EFULL; /* value overflowed */
    }

    /* resume suspended thread */
    if (!rt_list_isempty(&mq->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mq_urgent);

/**
 * @brief    This function will receive a message from message queue object,
 *           if there is no message in messagequeue object, the thread shall wait for a specified time.
 *
 * @note     If a message is available (entry > 0), the caller receives it and returns the copied length (> 0).
 *           If the queue is empty, the caller blocks until a message arrives or timeout elapses (-RT_ETIMEOUT).
 * @note     buffer 为出参缓冲区，size 为其容量；返回 len 为实际拷贝字节数（不超过单条消息长度与 size）。
 *           entry==0 且 timeout==0 返回 -RT_ETIMEOUT。取消息后槽还回 msg_queue_free，并可能唤醒阻塞的发送线程。
 *           prio 仅在 RT_USING_MESSAGEQUEUE_PRIORITY 且非 RT_NULL 时写入消息优先级。
 *
 * @param    mq is a pointer to the messagequeue object to be received.
 *
 * @param    buffer is the buffer to store the received message (output).
 *
 * @param    prio is optional output for message priority (larger value means higher priority);
 *           may be RT_NULL. Valid only when RT_USING_MESSAGEQUEUE_PRIORITY is enabled.
 *
 * @param    size is the capacity of buffer (Unit: Byte).
 *
 * @param    timeout is a timeout period (unit: an OS tick). If the message is unavailable, the thread will wait for
 *           the message in the queue up to the amount of time specified by this parameter.
 *
 * @param    suspend_flag status flag of the thread to be suspended.
 *
 *           NOTE:
 *           If use Macro RT_WAITING_FOREVER to set this parameter, which means that when the
 *           message is unavailable in the queue, the thread will be waiting forever.
 *           If use macro RT_WAITING_NO to set this parameter, which means that this
 *           function is non-blocking and will return immediately.
 *
 * @return   Return the real length of the message. When the return value is larger than zero, the operation is successful.
 *           If the return value is any other values, it means that the message queue receive failed.
 */
static rt_ssize_t _rt_mq_recv(rt_mq_t mq,
                              void *buffer,
                              rt_size_t size,
                              rt_int32_t *prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    struct rt_mq_message *msg;
    rt_uint32_t tick_delta;
    rt_err_t ret;
    rt_size_t len;

#ifndef RT_USING_MESSAGEQUEUE_PRIORITY
    RT_UNUSED(prio);
#endif

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* current context checking */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* initialize delta tick */
    tick_delta = 0;
    /* get current thread */
    thread = rt_thread_self();
    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* for non-blocking call */
    if (mq->entry == 0 && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_ETIMEOUT;
    }

    /* message queue is empty */
    /* 无消息则挂起在 parent.suspend_thread */
    while (mq->entry == 0)
    {
        /* reset error number in thread */
        thread->error = -RT_EINTR;

        /* no waiting, return timeout */
        if (timeout == 0)
        {
            /* enable interrupt */
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);

            thread->error = -RT_ETIMEOUT;

            return -RT_ETIMEOUT;
        }

        /* suspend current thread */
        ret = rt_thread_suspend_to_list(thread, &(mq->parent.suspend_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);
            return ret;
        }

        /* has waiting time, start thread timer */
        if (timeout > 0)
        {
            /* get the start tick of timer */
            tick_delta = rt_tick_get();

            LOG_D("set thread:%s to timer list",
                  thread->parent.name);

            /* reset the timeout of thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        /* re-schedule */
        rt_schedule();

        /* recv message */
        if (thread->error != RT_EOK)
        {
            /* return error */
            return thread->error;
        }

        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* if it's not waiting forever and then re-calculate timeout tick */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* get message from queue */
    /* 队头即最早/最高优先级消息（取决于是否启用优先级队列） */
    msg = (struct rt_mq_message *)mq->msg_queue_head;

    /* move message queue head */
    mq->msg_queue_head = msg->next;
    /* reach queue tail, set to NULL */
    if (mq->msg_queue_tail == msg)
        mq->msg_queue_tail = RT_NULL;

    /* decrease message entry */
    if(mq->entry > 0)
    {
        mq->entry --;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* get real message length */
    len = ((struct rt_mq_message *)msg)->length;

    if (len > size)
        len = size;
    /* copy message */
    /* 用户 buffer 不足时只拷贝前 size 字节 */
    rt_memcpy(buffer, GET_MESSAGEBYTE_ADDR(msg), len);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    if (prio != RT_NULL)
        *prio = msg->prio;
#endif
    level = rt_spin_lock_irqsave(&(mq->spinlock));
    /* put message to free list */
    /* 归还槽到空闲栈，供后续 send 使用 */
    msg->next = (struct rt_mq_message *)mq->msg_queue_free;
    mq->msg_queue_free = msg;

    /* resume suspended thread */
    if (!rt_list_isempty(&(mq->suspend_sender_thread)))
    {
        rt_susp_list_dequeue(&(mq->suspend_sender_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent)));

        rt_schedule();

        return len;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent)));

    return len;
}

/* 公共 API 不暴露 prio：传 (rt_int32_t *)0，与未启用 RT_USING_MESSAGEQUEUE_PRIORITY 时行为一致 */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, (rt_int32_t *)0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv);

/* 可被信号打断的阻塞接收（suspend_flag=RT_INTERRUPTIBLE） */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, (rt_int32_t *)0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv_interruptible);

/* 可被 kill 的阻塞接收（suspend_flag=RT_KILLABLE） */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, (rt_int32_t *)0, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mq_recv_killable);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
/* 显式带消息优先级的 send/recv 包装；prio 在 send 为整型优先级，在 recv 为可选出参指针 */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    return _rt_mq_send_wait(mq, buffer, size, prio, timeout, suspend_flag);
}
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag)
{
    return _rt_mq_recv(mq, buffer, size, prio, timeout, suspend_flag);
}
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */

/**
 * @brief    This function will set some extra attributions of a messagequeue object.
 *
 * @note     Currently this function only supports the RT_IPC_CMD_RESET command to reset the messagequeue.
 * @note     RESET 唤醒所有阻塞线程，将已排队消息逐节归还 msg_queue_free，entry 清零（与邮箱 reset 类似
 *           但更彻底回收链表节点）。
 *
 * @param    mq is a pointer to a messagequeue object.
 *
 * @param    cmd is a command used to configure some attributions of the messagequeue.
 *
 * @param    arg is the argument of the function to execute the command.
 *
 * @return   Return the operation status. When the return value is RT_EOK, the operation is successful.
 *           If the return value is any other values, it means that this function failed to execute.
 */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg)
{
    rt_base_t level;
    struct rt_mq_message *msg;

    RT_UNUSED(arg);

    /* parameter check */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* resume all waiting thread */
        rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
        /* also resume all message queue private suspended thread */
        rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

        /* release all message in the queue */
        /* 把仍在 FIFO/优先级链上的消息全部退回空闲链表 */
        while (mq->msg_queue_head != RT_NULL)
        {
            /* get message from queue */
            msg = (struct rt_mq_message *)mq->msg_queue_head;

            /* move message queue head */
            mq->msg_queue_head = msg->next;
            /* reach queue tail, set to NULL */
            if (mq->msg_queue_tail == msg)
                mq->msg_queue_tail = RT_NULL;

            /* put message to free list */
            msg->next = (struct rt_mq_message *)mq->msg_queue_free;
            mq->msg_queue_free = msg;
        }

        /* clean entry */
        mq->entry = 0;

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    /* 其它 cmd 未实现 */
    return -RT_ERROR;
}
RTM_EXPORT(rt_mq_control);

/**@}*/
#endif /* RT_USING_MESSAGEQUEUE */

/* 结束 Doxygen @addtogroup group_IPC */
/**@}*/
