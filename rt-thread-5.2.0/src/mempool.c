/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-05-27     Bernard      implement memory pool
 * 2006-06-03     Bernard      fix the thread timer init bug
 * 2006-06-30     Bernard      fix the allocate/free block bug
 * 2006-08-04     Bernard      add hook support
 * 2006-08-10     Bernard      fix interrupt bug in rt_mp_alloc
 * 2010-07-13     Bernard      fix RT_ALIGN issue found by kuronca
 * 2010-10-26     yi.qiu       add module support in rt_mp_delete
 * 2011-01-24     Bernard      add object allocation check.
 * 2012-03-22     Bernard      fix align issue in rt_mp_init and rt_mp_create.
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to mempool.c
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       fix spinlock assert
 *
 * ---------------------------------------------------------------------------
 * 模块说明（内存池 RT_USING_MEMPOOL）
 * ---------------------------------------------------------------------------
 * 内存池将一段连续内存划分为若干等长「槽」：每槽布局为
 *   [sizeof(void*) 元数据区][block_size 用户数据区]
 * 空闲时元数据区存下一空闲槽指针，形成单链表 block_list；分配后该字改写为
 * 所属 struct rt_mempool *，供 rt_mp_free 反查池对象。rt_mp_alloc 返回的是
 * 用户区首址（跳过元数据指针宽度）。
 *
 * 与 memheap/small mem 不同：块大小固定、无分裂合并；分配/释放 O(1)（仅链表
 * 头插），适合 DMA 缓冲、固定对象池等场景。无块且 time!=0 时线程挂起到
 * suspend_thread，由 rt_mp_free 唤醒。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MEMPOOL

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_mp_alloc_hook)(struct rt_mempool *mp, void *block);
static void (*rt_mp_free_hook)(struct rt_mempool *mp, void *block);

/**
 * @addtogroup group_Hook
 */

/**@{*/

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is allocated from the memory pool.
 *
 * @param hook the hook function
 * @note 中文：在自旋锁释放之后调用；hook 内勿再对同一 mp 做可能阻塞的 alloc。
 */
void rt_mp_alloc_sethook(void (*hook)(struct rt_mempool *mp, void *block))
{
    rt_mp_alloc_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when a memory
 *        block is released to the memory pool.
 *
 * @param hook the hook function
 * @note 中文：在加锁之前调用，参数 block 为用户区指针。
 */
void rt_mp_free_sethook(void (*hook)(struct rt_mempool *mp, void *block))
{
    rt_mp_free_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_MM
 */

/**@{*/

/**
 * @brief  This function will initialize a memory pool object, normally which is used
 *         for static object.
 *
 * @param  mp is the memory pool object.
 *
 * @param  name is the name of the memory pool.
 *
 * @param  start is the start address of the memory pool.
 *
 * @param  size is the total size of the memory pool.
 *
 * @param  block_size is the size for each block (user payload, aligned).
 *
 * @return RT_EOK
 * @note   中文：静态池；start/size 由调用方提供，块数 = size / (block_size+指针宽)。
 *         初始化把各槽串成空闲链，最后一槽 next 置 RT_NULL。
 */
rt_err_t rt_mp_init(struct rt_mempool *mp,
                    const char        *name,
                    void              *start,
                    rt_size_t          size,
                    rt_size_t          block_size)
{
    rt_uint8_t *block_ptr;
    rt_size_t offset;

    /* parameter check */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(name != RT_NULL);
    RT_ASSERT(start != RT_NULL);
    RT_ASSERT(size > 0 && block_size > 0);

    /* initialize object */
    rt_object_init(&(mp->parent), RT_Object_Class_MemPool, name);

    /* initialize memory pool */
    mp->start_address = start;
    mp->size = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);

    /* align the block size */
    block_size = RT_ALIGN(block_size, RT_ALIGN_SIZE);
    mp->block_size = block_size;

    /* align to align size byte */
    mp->block_total_count = mp->size / (mp->block_size + sizeof(rt_uint8_t *));
    mp->block_free_count  = mp->block_total_count;

    /* initialize suspended thread list */
    rt_list_init(&(mp->suspend_thread));

    /* 每槽首部 void* 存下一槽地址；用户可用区紧随其后，长度为 block_size */
    block_ptr = (rt_uint8_t *)mp->start_address;
    for (offset = 0; offset < mp->block_total_count; offset ++)
    {
        *(rt_uint8_t **)(block_ptr + offset * (block_size + sizeof(rt_uint8_t *))) =
            (rt_uint8_t *)(block_ptr + (offset + 1) * (block_size + sizeof(rt_uint8_t *)));
    }

    *(rt_uint8_t **)(block_ptr + (offset - 1) * (block_size + sizeof(rt_uint8_t *))) =
        RT_NULL;

    mp->block_list = block_ptr;
    rt_spin_lock_init(&(mp->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mp_init);

/**
 * @brief  This function will detach a memory pool from system object management.
 *
 * @param  mp is the memory pool object.
 *
 * @return RT_EOK
 * @note   中文：唤醒所有阻塞在池上的线程；不释放 start_address 指向的静态内存。
 */
rt_err_t rt_mp_detach(struct rt_mempool *mp)
{
    rt_base_t level;

    /* parameter check */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mp->parent) == RT_Object_Class_MemPool);
    RT_ASSERT(rt_object_is_systemobject(&mp->parent));

    level = rt_spin_lock_irqsave(&(mp->spinlock));
    /* wake up all suspended threads */
    rt_susp_list_resume_all(&mp->suspend_thread, RT_ERROR);

    /* detach object */
    rt_object_detach(&(mp->parent));
    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mp_detach);

#ifdef RT_USING_HEAP
/**
 * @brief This function will create a mempool object and allocate the memory pool from
 *        heap.
 *
 * @param name is the name of memory pool.
 *
 * @param block_count is the count of blocks in memory pool.
 *
 * @param block_size is the size for each block.
 *
 * @return the created mempool object
 * @note   中文：从堆分配对象与池内存，勿在中断里调用（RT_DEBUG_NOT_IN_INTERRUPT）。
 *         失败时若池内存申请失败会删除已分配的内核对象。
 */
rt_mp_t rt_mp_create(const char *name,
                     rt_size_t   block_count,
                     rt_size_t   block_size)
{
    rt_uint8_t *block_ptr;
    struct rt_mempool *mp;
    rt_size_t offset;

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* parameter check */
    RT_ASSERT(name != RT_NULL);
    RT_ASSERT(block_count > 0 && block_size > 0);

    /* allocate object */
    mp = (struct rt_mempool *)rt_object_allocate(RT_Object_Class_MemPool, name);
    /* allocate object failed */
    if (mp == RT_NULL)
        return RT_NULL;

    /* initialize memory pool */
    block_size     = RT_ALIGN(block_size, RT_ALIGN_SIZE);
    mp->block_size = block_size;
    mp->size       = (block_size + sizeof(rt_uint8_t *)) * block_count;

    /* allocate memory */
    mp->start_address = rt_malloc((block_size + sizeof(rt_uint8_t *)) *
                                  block_count);
    if (mp->start_address == RT_NULL)
    {
        /* no memory, delete memory pool object */
        rt_object_delete(&(mp->parent));

        return RT_NULL;
    }

    mp->block_total_count = block_count;
    mp->block_free_count  = mp->block_total_count;

    /* initialize suspended thread list */
    rt_list_init(&(mp->suspend_thread));

    /* initialize free block list */
    block_ptr = (rt_uint8_t *)mp->start_address;
    for (offset = 0; offset < mp->block_total_count; offset ++)
    {
        *(rt_uint8_t **)(block_ptr + offset * (block_size + sizeof(rt_uint8_t *)))
            = block_ptr + (offset + 1) * (block_size + sizeof(rt_uint8_t *));
    }

    *(rt_uint8_t **)(block_ptr + (offset - 1) * (block_size + sizeof(rt_uint8_t *)))
        = RT_NULL;

    mp->block_list = block_ptr;
    rt_spin_lock_init(&(mp->spinlock));

    return mp;
}
RTM_EXPORT(rt_mp_create);

/**
 * @brief This function will delete a memory pool and release the object memory.
 *
 * @param mp is the memory pool object.
 *
 * @return RT_EOK
 * @note   中文：先唤醒阻塞线程，再 rt_free(start_address)，最后删除内核对象。
 */
rt_err_t rt_mp_delete(rt_mp_t mp)
{
    rt_base_t level;

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* parameter check */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mp->parent) == RT_Object_Class_MemPool);
    RT_ASSERT(rt_object_is_systemobject(&mp->parent) == RT_FALSE);

    level = rt_spin_lock_irqsave(&(mp->spinlock));
    /* wake up all suspended threads */
    rt_susp_list_resume_all(&mp->suspend_thread, RT_ERROR);

    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    /* release allocated room */
    rt_free(mp->start_address);

    /* detach object */
    rt_object_delete(&(mp->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mp_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief This function will allocate a block from memory pool.
 *
 * @param mp is the memory pool object.
 *
 * @param time is the maximum waiting time for allocating memory.
 *             - 0 for not waiting, allocating memory immediately.
 *
 * @return the allocated memory block or RT_NULL on allocated failed.
 * @note   中文：无空闲块且 time==0 返回 RT_NULL 并置 errno -RT_ETIMEOUT；阻塞路径
 *         使用线程定时器扣减剩余 tick。返回前在槽首写入 mp 指针供 rt_mp_free 使用。
 */
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time)
{
    rt_uint8_t *block_ptr;
    rt_base_t level;
    struct rt_thread *thread;
    rt_uint32_t before_sleep = 0;

    /* parameter check */
    RT_ASSERT(mp != RT_NULL);

    /* get current thread */
    thread = rt_thread_self();

    level = rt_spin_lock_irqsave(&(mp->spinlock));

    while (mp->block_free_count == 0)
    {
        /* memory block is unavailable. */
        if (time == 0)
        {
            rt_spin_unlock_irqrestore(&(mp->spinlock), level);

            rt_set_errno(-RT_ETIMEOUT);

            return RT_NULL;
        }

        RT_DEBUG_NOT_IN_INTERRUPT;

        thread->error = RT_EOK;

        /* need suspend thread */
        rt_thread_suspend_to_list(thread, &mp->suspend_thread, RT_IPC_FLAG_FIFO, RT_UNINTERRUPTIBLE);

        if (time > 0)
        {
            /* get the start tick of timer */
            before_sleep = rt_tick_get();

            /* init thread timer and start it */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &time);
            rt_timer_start(&(thread->thread_timer));
        }

        /* enable interrupt */
        rt_spin_unlock_irqrestore(&(mp->spinlock), level);

        /* do a schedule */
        rt_schedule();

        if (thread->error != RT_EOK)
            return RT_NULL;

        if (time > 0)
        {
            time -= rt_tick_get() - before_sleep;
            if (time < 0)
                time = 0;
        }
        level = rt_spin_lock_irqsave(&(mp->spinlock));
    }

    /* memory block is available. decrease the free block counter */
    mp->block_free_count--;

    /* 从空闲链表头弹出一槽 */
    block_ptr = mp->block_list;
    RT_ASSERT(block_ptr != RT_NULL);

    mp->block_list = *(rt_uint8_t **)block_ptr;

    /* 元数据字改写为池指针（覆盖原 next），与空闲链语义切换 */
    *(rt_uint8_t **)block_ptr = (rt_uint8_t *)mp;

    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_mp_alloc_hook,
                        (mp, (rt_uint8_t *)(block_ptr + sizeof(rt_uint8_t *))));

    return (rt_uint8_t *)(block_ptr + sizeof(rt_uint8_t *));
}
RTM_EXPORT(rt_mp_alloc);

/**
 * @brief This function will release a memory block.
 *
 * @param block the address of memory block to be released.
 * @note 中文：block 须为 rt_mp_alloc 返回值；通过 block 前一字取 mp，再头插回
 *       空闲链。若有阻塞线程则释放锁后 schedule 唤醒一个。
 */
void rt_mp_free(void *block)
{
    rt_uint8_t **block_ptr;
    struct rt_mempool *mp;
    rt_base_t level;

    /* parameter check */
    if (block == RT_NULL) return;

    /* 用户区紧邻前方即为分配时写入的 mp 指针 */
    block_ptr = (rt_uint8_t **)((rt_uint8_t *)block - sizeof(rt_uint8_t *));
    mp        = (struct rt_mempool *)*block_ptr;

    RT_OBJECT_HOOK_CALL(rt_mp_free_hook, (mp, block));

    level = rt_spin_lock_irqsave(&(mp->spinlock));

    /* increase the free block count */
    mp->block_free_count ++;

    /* 头插回空闲链：*block_ptr 恢复为「下一空闲槽」指针语义 */
    *block_ptr = mp->block_list;
    mp->block_list = (rt_uint8_t *)block_ptr;

    if (rt_susp_list_dequeue(&mp->suspend_thread, RT_EOK))
    {
        rt_spin_unlock_irqrestore(&(mp->spinlock), level);

        /* do a schedule */
        rt_schedule();

        return;
    }
    rt_spin_unlock_irqrestore(&(mp->spinlock), level);
}
RTM_EXPORT(rt_mp_free);

/**@}*/

#endif /* RT_USING_MEMPOOL */
