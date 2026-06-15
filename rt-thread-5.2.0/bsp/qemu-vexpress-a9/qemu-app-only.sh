#!/bin/bash
# Quick start App only (no QBoot chain) — for debugging App
if [ ! -f "sd.bin" ]; then
dd if=/dev/zero of=sd.bin bs=1024 count=65536
fi

export QEMU_AUDIO_DRV=none

PFLASH=""
if [ -f "flash0.bin" ] && [ "$(stat -c%s flash0.bin)" -ge 67108864 ]; then
    PFLASH="-drive if=pflash,format=raw,file=flash0.bin"
fi

# Use ELF so QEMU loads at link address 0x60010000; -smp 1 matches non-SMP build.
exec qemu-system-arm -M vexpress-a9 -smp 1 \
    -kernel rtthread.elf \
    $PFLASH \
    -drive if=sd,format=raw,file=sd.bin \
    -nographic
