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
    rt_kprintf("Hello RT-Thread! chenpan20260424\n");

    return 0;
}
