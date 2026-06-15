/*
 * Minimal board.h for QBoot project (avoids mm_aspace include chain in libcpu).
 */
#ifndef __BOARD_H__
#define __BOARD_H__

#include <rtconfig.h>
#include "realview.h"
#include "vexpress_a9.h"

extern int __bss_end;
#define HEAP_BEGIN      ((void *)&__bss_end)
#define HEAP_END        ((void *)(0x61000000 + 2 * 1024 * 1024))

void rt_hw_board_init(void);

#endif
