/*
 * OTA partition layout on QEMU vexpress-a9 NOR Flash0 (memory-mapped @ 0x40000000).
 *
 *  nor_flash0 (16MB used of 64MB emulated pflash)
 *    bl        512KB   @ offset 0
 *    app       4MB     @ offset 512KB
 *    download  4MB     staging for .rbl OTA packages
 *    param     512KB   boot flags / version
 */
#ifndef __OTA_PARTITION_H__
#define __OTA_PARTITION_H__

#define RT_NOR0_FAL_LEN             (16 * 1024 * 1024)
#define RT_NOR0_SECTOR_SIZE         (256 * 1024)

#define RT_BOOT_PART_SIZE           (512 * 1024)
#define RT_APP_PART_SIZE            (4 * 1024 * 1024)
#define RT_DOWNLOAD_PART_SIZE       (4 * 1024 * 1024)
#define RT_PARAM_PART_SIZE          (512 * 1024)

#define RT_BOOT_PART_OFFSET         0
#define RT_APP_PART_OFFSET          RT_BOOT_PART_SIZE
#define RT_DOWNLOAD_PART_OFFSET     (RT_BOOT_PART_OFFSET + RT_APP_PART_SIZE)
#define RT_PARAM_PART_OFFSET        (RT_DOWNLOAD_PART_OFFSET + RT_DOWNLOAD_PART_SIZE)

/* Physical NOR base (QEMU pflash NOR0) */
#define RT_NOR0_PHYS_BASE           0x40000000

/* App storage in NOR vs runtime load address in RAM */
#define RT_APP_PART_ADDR            (RT_NOR0_PHYS_BASE + RT_APP_PART_OFFSET)
#define RT_APP_LOAD_ADDR            0x60010000

#endif /* __OTA_PARTITION_H__ */
