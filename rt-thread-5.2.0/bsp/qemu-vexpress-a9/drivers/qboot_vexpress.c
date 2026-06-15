/*
 * QBoot jump-to-app for QEMU vexpress-a9 (Cortex-A9).
 * App is linked for RAM @ RT_APP_LOAD_ADDR; image stored in NOR app partition.
 */
#include <rtthread.h>
#include <rthw.h>
#include <fal.h>
#include <qboot.h>
#include "ota_partition.h"

static void qbt_reset_to_app(rt_uint32_t reset_addr)
{
    rt_hw_interrupt_disable();
    rt_hw_cpu_icache_ops(RT_HW_CACHE_INVALIDATE, (void *)RT_APP_LOAD_ADDR, RT_APP_PART_SIZE);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)RT_APP_LOAD_ADDR, RT_APP_PART_SIZE);

    /* Never returns: branch to app _reset with IRQ/FIQ masked */
    __asm volatile (
        "cpsid if\n"
        "mov pc, %0\n"
        : : "r" (reset_addr) : "memory"
    );
}

void qbt_jump_to_app(void)
{
    const struct fal_partition *part;
    rt_uint32_t load_addr = RT_APP_LOAD_ADDR;
    rt_uint32_t reset_handler;
    rt_size_t read_len;

    part = fal_partition_find(QBOOT_APP_PART_NAME);
    if (part == RT_NULL)
    {
        rt_kprintf("QBoot: app partition not found\n");
        return;
    }

    read_len = part->len;
    if (read_len > RT_APP_PART_SIZE)
    {
        read_len = RT_APP_PART_SIZE;
    }

    rt_kprintf("QBoot: load app from NOR -> 0x%08x (%d KB) ...\n",
               load_addr, read_len / 1024);

    if (fal_partition_read(part, 0, (rt_uint8_t *)load_addr, read_len) < 0)
    {
        rt_kprintf("QBoot: read app partition failed\n");
        return;
    }

    reset_handler = *(volatile rt_uint32_t *)(load_addr + 0x20);
    if (reset_handler < load_addr || reset_handler >= load_addr + read_len)
    {
        rt_kprintf("QBoot: invalid reset handler 0x%08x\n", reset_handler);
        return;
    }

    rt_kprintf("QBoot: jump to application @ 0x%08x ...\n", reset_handler);
    qbt_reset_to_app(reset_handler);

    rt_kprintf("QBoot: jump failed\n");
}
