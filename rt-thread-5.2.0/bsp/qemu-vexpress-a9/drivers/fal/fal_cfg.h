/*
 * FAL partition table for QEMU vexpress-a9 (QBoot-compatible layout).
 */
#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#include <rtconfig.h>
#include "ota_partition.h"

extern const struct fal_flash_dev nor_flash0;

#define FAL_FLASH_DEV_TABLE             \
{                                       \
    &nor_flash0,                        \
}

#ifdef FAL_PART_HAS_TABLE_CFG
#define FAL_PART_TABLE                                                                                  \
{                                                                                                       \
    {FAL_PART_MAGIC_WROD,          "bl", "nor_flash0", RT_BOOT_PART_OFFSET,     RT_BOOT_PART_SIZE, 0}, \
    {FAL_PART_MAGIC_WROD,         "app", "nor_flash0", RT_APP_PART_OFFSET,      RT_APP_PART_SIZE, 0}, \
    {FAL_PART_MAGIC_WROD,   "download", "nor_flash0", RT_DOWNLOAD_PART_OFFSET, RT_DOWNLOAD_PART_SIZE, 0}, \
    {FAL_PART_MAGIC_WROD,       "param", "nor_flash0", RT_PARAM_PART_OFFSET,    RT_PARAM_PART_SIZE, 0}, \
}
#endif /* FAL_PART_HAS_TABLE_CFG */

#endif /* _FAL_CFG_H_ */
