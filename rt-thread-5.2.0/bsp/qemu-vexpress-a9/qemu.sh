if [ ! -f "sd.bin" ]; then
dd if=/dev/zero of=sd.bin bs=1024 count=65536
fi

if [ ! -f "flash0.bin" ] || [ "$(stat -c%s flash0.bin)" -lt 67108864 ]; then
echo "Run ../qemu-vexpress-a9-boot/flash_image.sh first"
exit 1
fi

export QEMU_AUDIO_DRV=none

qemu-system-arm --version
qemu-system-arm -M vexpress-a9 -smp 1 \
    -drive if=pflash,format=raw,file=flash0.bin \
    -device loader,file=../qemu-vexpress-a9-boot/qboot.bin,addr=0x40000000,cpu-num=0 \
    -drive if=sd,format=raw,file=sd.bin \
    -serial stdio
