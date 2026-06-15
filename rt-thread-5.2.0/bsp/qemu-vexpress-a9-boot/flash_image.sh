#!/bin/bash
# Program flash0.bin: bl @ 0, app @ 512KB
set -e
BSP_APP=../qemu-vexpress-a9
BOOT_BIN=qboot.bin
APP_BIN=rtthread.bin
FLASH=flash0.bin
APP_OFFSET=524288

if [ ! -f "$BOOT_BIN" ]; then
    echo "Build boot first: cd qemu-vexpress-a9-boot && scons"
    exit 1
fi
if [ ! -f "$BSP_APP/$APP_BIN" ]; then
    echo "Build app first: cd $BSP_APP && scons"
    exit 1
fi

dd if=/dev/zero of="$FLASH" bs=1024 count=$((64 * 1024))
dd if="$BOOT_BIN" of="$FLASH" conv=notrunc
dd if="$BSP_APP/$APP_BIN" of="$FLASH" bs=1 seek=$APP_OFFSET conv=notrunc
cp "$FLASH" "$BSP_APP/$FLASH"
echo "flash0.bin ready: qboot @ 0, app @ 0x80000 ($APP_OFFSET)"
echo "Copied to $BSP_APP/$FLASH for qemu-vexpress-a9 scripts"
