if [ ! -f "sd.bin" ]; then
dd if=/dev/zero of=sd.bin bs=1024 count=65536
fi

if [ ! -f "flash0.bin" ] || [ "$(stat -c%s flash0.bin)" -lt 67108864 ]; then
echo "Creating flash0.bin (64MB)..."
dd if=/dev/zero of=flash0.bin bs=1024 count=$((64 * 1024))
fi

export QEMU_AUDIO_DRV=none

qemu-system-arm -M vexpress-a9 -smp cpus=2 \
    -kernel rtthread.bin \
    -drive if=pflash,format=raw,file=flash0.bin \
    -drive if=sd,format=raw,file=sd.bin \
    -nographic -S -s
