/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020/12/31     Bernard      Add license info
 */

#include <stdint.h>
#include <stdio.h>
#include <rtthread.h>
#include <finsh.h>

#define ADD_THREAD_COUNT       4
#define ADD_PER_THREAD_TIMES   10000

static rt_int32_t g_shared_sum = 0;
static rt_int32_t g_finished_threads = 0;
static struct rt_mutex g_sum_mutex;

static void add_worker_entry(void *parameter)
{
    rt_int32_t loop_times = (rt_int32_t)(rt_ubase_t)parameter;
    const char *thread_name = rt_thread_self()->parent.name;
    rt_int32_t current_sum = 0;

    rt_kprintf("[%s] start add, loop_times=%d\n", thread_name, loop_times);

    for (rt_int32_t i = 0; i < loop_times; i++)
    {
        rt_mutex_take(&g_sum_mutex, RT_WAITING_FOREVER);
        g_shared_sum += 1;
        current_sum = g_shared_sum;
        rt_mutex_release(&g_sum_mutex);

        if (((i + 1) % 2500) == 0)
        {
            rt_kprintf("[%s] progress=%d, accumulated_sum=%d\n", thread_name, i + 1, current_sum);
            rt_thread_mdelay(1000);
        }
    }

    rt_mutex_take(&g_sum_mutex, RT_WAITING_FOREVER);
    g_finished_threads += 1;
    current_sum = g_shared_sum;
    rt_mutex_release(&g_sum_mutex);

    rt_kprintf("[%s] finished, accumulated_sum=%d\n", thread_name, current_sum);
}

/* 在 msh 里输入命令名可手动执行一次（与 main 里启动打印无关） */
static int cmd_run_once(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("run_once: executed from msh\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_run_once, run_once, Run application hook once from msh);

int main(void)
{
    rt_thread_t threads[ADD_THREAD_COUNT] = {0};
    char thread_names[ADD_THREAD_COUNT][RT_NAME_MAX] = {0};
    rt_int32_t expected_sum = ADD_THREAD_COUNT * ADD_PER_THREAD_TIMES;

    rt_kprintf("Hello RT-Thread! chenpan20260424\n");

    if (rt_mutex_init(&g_sum_mutex, "sum_mtx", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_kprintf("mutex init failed\n");
        return -1;
    }

    g_shared_sum = 0;
    g_finished_threads = 0;

    for (rt_int32_t i = 0; i < ADD_THREAD_COUNT; i++)
    {
        rt_snprintf(thread_names[i], RT_NAME_MAX, "add%02d", i);
        threads[i] = rt_thread_create(thread_names[i],
                                      add_worker_entry,
                                      (void *)(rt_ubase_t)ADD_PER_THREAD_TIMES,
                                      1024,
                                      20,
                                      10);
        if (threads[i] == RT_NULL)
        {
            rt_kprintf("thread create failed at index %d\n", i);
            return -1;
        }
        rt_thread_startup(threads[i]);
    }

    while (1)
    {
        rt_int32_t local_finished = 0;

        rt_mutex_take(&g_sum_mutex, RT_WAITING_FOREVER);
        local_finished = g_finished_threads;
        rt_mutex_release(&g_sum_mutex);

        if (local_finished >= ADD_THREAD_COUNT)
        {
            break;
        }
        rt_thread_mdelay(10);
    }

    rt_kprintf("multi-thread add done, expected=%d, actual=%d\n", expected_sum, g_shared_sum);

    return 0;
}
