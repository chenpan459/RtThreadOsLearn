/*
 * FAL port: QEMU vexpress-a9 NOR Flash0 (memory-mapped pflash @ 0x40000000).
 */
#include <fal.h>
#include <string.h>
#include "ota_partition.h"

static int nor0_init(void)
{
    return 0;
}

static int nor0_read(long offset, rt_uint8_t *buf, rt_size_t size)
{
    const rt_uint8_t *src = (const rt_uint8_t *)(RT_NOR0_PHYS_BASE + offset);

    if (offset < 0 || (rt_size_t)offset + size > RT_NOR0_FAL_LEN)
    {
        return -1;
    }

    memcpy(buf, src, size);
    return (int)size;
}

static int nor0_write(long offset, const rt_uint8_t *buf, rt_size_t size)
{
    rt_uint8_t *dst = (rt_uint8_t *)(RT_NOR0_PHYS_BASE + offset);

    if (offset < 0 || (rt_size_t)offset + size > RT_NOR0_FAL_LEN)
    {
        return -1;
    }

    memcpy(dst, buf, size);
    return (int)size;
}

static int nor0_erase(long offset, rt_size_t size)
{
    rt_uint8_t *base = (rt_uint8_t *)(RT_NOR0_PHYS_BASE + offset);
    rt_size_t sector = RT_NOR0_SECTOR_SIZE;

    if (offset < 0 || (rt_size_t)offset + size > RT_NOR0_FAL_LEN)
    {
        return -1;
    }

    for (rt_size_t pos = 0; pos < size; pos += sector)
    {
        memset(base + pos, 0xFF, sector);
    }

    return (int)size;
}

const struct fal_flash_dev nor_flash0 =
{
    .name       = "nor_flash0",
    .addr       = 0,
    .len        = RT_NOR0_FAL_LEN,
    .blk_size   = RT_NOR0_SECTOR_SIZE,
    .ops        = {nor0_init, nor0_read, nor0_write, nor0_erase},
    .write_gran = 1,
};
