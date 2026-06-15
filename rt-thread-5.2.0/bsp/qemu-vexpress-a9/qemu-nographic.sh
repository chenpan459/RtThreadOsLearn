if [ ! -f "sd.bin" ]; then
dd if=/dev/zero of=sd.bin bs=1024 count=65536
fi

if [ ! -f "flash0.bin" ] || [ "$(stat -c%s flash0.bin)" -lt 67108864 ]; then
echo "Run ../qemu-vexpress-a9-boot/flash_image.sh to create flash0.bin with boot+app"
exit 1
fi

if [ ! -f "../qemu-vexpress-a9-boot/qboot.bin" ]; then
echo "Build boot: cd ../qemu-vexpress-a9-boot && scons"
exit 1
fi

export QEMU_AUDIO_DRV=none

echo "Starting QEMU (Ctrl+A then X to quit) ..."
# Non-SMP build: use one CPU. loader starts QBoot XIP @ NOR 0x40000000.
exec qemu-system-arm -M vexpress-a9 -smp 1 \
    -drive if=pflash,format=raw,file=flash0.bin \
    -device loader,file=../qemu-vexpress-a9-boot/qboot.bin,addr=0x40000000,cpu-num=0 \
    -drive if=sd,format=raw,file=sd.bin \
    -nographic
