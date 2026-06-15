# QEMU vexpress-a9 QBoot 工程

从 NOR `bl` 分区（`0x40000000`）启动 QBoot，校验 `download` 分区后搬运到 `app`，再加载 App 到 RAM `0x60010000` 运行。

## 编译

```bash
# 1. Boot
cd bsp/qemu-vexpress-a9-boot
scons -j8
# 产物: qboot.bin (~214KB, 链接 0x40000000)

# 2. App
cd ../qemu-vexpress-a9
scons -j8
# 产物: rtthread.bin (链接 0x60010000，存入 NOR app 分区)
```

## 烧录 flash0.bin

```bash
cd bsp/qemu-vexpress-a9-boot
chmod +x flash_image.sh
./flash_image.sh
# 生成 ../qemu-vexpress-a9/flash0.bin:
#   qboot.bin  @ offset 0
#   rtthread.bin @ offset 512KB (0x80000)
```

## 运行（无 -kernel）

```bash
cd ../qemu-vexpress-a9
./qemu-nographic.sh
```

QEMU 通过 `loader` 从 `0x40000000` 启动 QBoot，pflash 文件持久化 OTA 数据。

## 与 App 工程差异

| 项目 | Boot | App |
|------|------|-----|
| QBoot | ✅ | ❌ |
| ota_downloader | ❌ | ✅ |
| FinSH/网络 | 最小化 | 完整 |
| 链接地址 | `0x40000000` (NOR bl) | `0x60010000` (RAM) |
