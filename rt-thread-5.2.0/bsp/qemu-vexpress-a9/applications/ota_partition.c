/*
 * OTA / FAL partition demo for QEMU vexpress-a9.
 *
 * QEMU loads firmware into RAM via -kernel; NOR partitions persist via pflash drives.
 * Use ymodem_ota (MSH) to write .rbl into download partition.
 */
#include <rtthread.h>
#include <fal.h>
#include "ota_partition.h"

static void ota_show_partitions(void)
{
    rt_size_t count = 0;
    const struct fal_partition *table = fal_get_partition_table(&count);

    rt_kprintf("QEMU NOR0 partitions (phys 0x%08x, sector %dKB):\n",
               RT_NOR0_PHYS_BASE, RT_NOR0_SECTOR_SIZE / 1024);
    rt_kprintf("  name       offset      size\n");
    rt_kprintf("  bl         0x%08x  %dKB\n", RT_BOOT_PART_OFFSET, RT_BOOT_PART_SIZE / 1024);
    rt_kprintf("  app        0x%08x  %dKB\n", RT_APP_PART_OFFSET, RT_APP_PART_SIZE / 1024);
    rt_kprintf("  download   0x%08x  %dKB  <- OTA staging (.rbl)\n",
               RT_DOWNLOAD_PART_OFFSET, RT_DOWNLOAD_PART_SIZE / 1024);
    rt_kprintf("  param      0x%08x  %dKB\n", RT_PARAM_PART_OFFSET, RT_PARAM_PART_SIZE / 1024);
    rt_kprintf("--- FAL table (%d entries) ---\n", count);

    for (rt_size_t i = 0; i < count; i++)
    {
        rt_kprintf("  %-10s dev=%-12s off=0x%08x len=0x%08x\n",
                   table[i].name,
                   table[i].flash_name,
                   (unsigned)table[i].offset,
                   (unsigned)table[i].len);
    }
}

static int cmd_ota_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ota_show_partitions();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ota_info, ota_info, Show QBoot-style NOR partition layout);

static int ota_partition_init(void)
{
    ota_show_partitions();
    rt_kprintf("OTA commands:\n");
    rt_kprintf("  fal probe nor_flash0   - probe NOR flash device\n");
    rt_kprintf("  fal probe download     - probe download partition\n");
    rt_kprintf("  fal show               - show probed partition (after probe)\n");
    rt_kprintf("  ymodem_ota             - YModem receive .rbl -> download\n");
    return 0;
}
INIT_APP_EXPORT(ota_partition_init);

static int cmd_ota_probe(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const struct fal_flash_dev *flash = fal_flash_device_find("nor_flash0");
    const struct fal_partition *part = fal_partition_find("download");

    if (flash == RT_NULL)
    {
        rt_kprintf("nor_flash0 NOT found, fal_init may have failed\n");
        return -1;
    }

    rt_kprintf("nor_flash0 OK | len=%d | blk=%d\n", flash->len, flash->blk_size);

    if (part == RT_NULL)
    {
        rt_kprintf("download partition NOT found\n");
        return -1;
    }

    rt_kprintf("download OK | offset=0x%08x | len=0x%08x\n",
               (unsigned)part->offset, (unsigned)part->len);
    rt_kprintf("Now run: fal probe download && fal show\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ota_probe, ota_probe, Probe nor_flash0 and download partition);
