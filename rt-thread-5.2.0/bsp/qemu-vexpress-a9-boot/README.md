# QEMU vexpress-a9 QBoot 工程

从 NOR `bl` 分区（`0x40000000`）启动 QBoot，校验 `download` 分区后搬运到 `app`，再加载 App 到 RAM `0x60010000` 运行。

## 编译

```bash
# 1. Boot
cd bsp/qemu-vexpress-a9-boot
scons -j8
# 产物: qboot.bin (~214KB, 链接 0x40000000)
```

### QBoot 代码从哪里来？

Boot 工程**没有**把 `qboot.c` 复制到本目录，而是通过 `board/boot_pkgs.SConscript` **引用 App 工程下的 packages**：

| 组件 | 源码路径（相对 `qemu-vexpress-a9`） |
|------|-------------------------------------|
| QBoot 核心 | `packages/qboot-latest/src/qboot.c` |
| QuickLZ 适配 | `packages/qboot-latest/src/qboot_quicklz.c` |
| CRC32 | `packages/crclib-v1.02/src/crc32.c` |
| QuickLZ | `packages/quicklz-v1.0.1/` |
| 跳转 App | `drivers/qboot_vexpress.c`（`board/SConscript` 引用父 BSP） |
| FAL / NOR | `drivers/fal/`（同上） |

因此 `scons` 日志里**不一定出现** `CC .../qboot.c`：`.o` 常生成在 **`qemu-vexpress-a9/packages/qboot-latest/src/qboot.o`**，增量编译时若未改动则不会打印 CC 行。

**确认已链入 QBoot：**

```bash
arm-none-eabi-nm qboot.elf | grep -E 'qbt_startup|qbt_jump_to_app|qboot'
grep qboot-latest/src/qboot.o qboot.map
```

运行时应看到 `Qboot startup ...`（你已在 `qemu-nographic.sh` 中验证）。

```bash
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
