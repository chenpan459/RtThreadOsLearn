# QEMU vexpress-a9 OTA 双工程说明

本 BSP 采用 **Boot + App** 双工程，NOR Flash0 上为 QBoot 兼容分区，由 pflash `flash0.bin` 持久化。

## 工程目录

| 目录 | 说明 |
|------|------|
| `qemu-vexpress-a9` | **App**（RAM `0x60010000`，含 `ymodem_ota`） |
| `qemu-vexpress-a9-boot` | **Boot**（NOR bl `0x40000000`，QBoot） |

## NOR Flash0 分区（16MB 逻辑布局 / 64MB pflash 文件）

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| `bl` | 0 | 512KB | QBoot |
| `app` | 512KB | 4MB | App 镜像（`.bin` 存储区） |
| `download` | 4.5MB | 4MB | OTA 暂存 `.rbl` |
| `param` | 8.5MB | 512KB | 参数 |

## 编译与运行

```bash
# Boot
cd bsp/qemu-vexpress-a9-boot && scons -j8

# App
cd ../qemu-vexpress-a9 && scons -j8

# 合成 flash0.bin（boot + app）
cd ../qemu-vexpress-a9-boot && ./flash_image.sh

# 启动（无 -kernel，从 NOR bl 启动 QBoot）
cd ../qemu-vexpress-a9 && ./qemu-nographic.sh
```

## 若启动后无输出（黑屏）

1. **先确认 App 能跑**（不经过 QBoot）：
   ```bash
   cd bsp/qemu-vexpress-a9
   chmod +x qemu-app-only.sh
   ./qemu-app-only.sh
   ```
   应出现 RT-Thread 启动横幅。若这里也无输出，检查 `qemu-system-arm` 是否安装正常。

2. **重新编译 Boot**（已修复 RAM 与 App 重叠问题）：
   ```bash
   cd bsp/qemu-vexpress-a9-boot
   scons -c && scons && ./flash_image.sh
   cd ../qemu-vexpress-a9
   ./qemu-nographic.sh
   ```

3. **退出 QEMU**：`Ctrl+A` 松开后按 `X`（不是 `exit`）。

4. QBoot 首次启动会校验分区，**等待 10～30 秒** 再看是否出现日志。

1. App 运行：`ymodem_ota` 发送 `.rbl` → `download` 分区
2. 重启 QEMU → QBoot 校验并写入 `app` 分区
3. QBoot 将 `app` 分区镜像复制到 RAM `0x60010000` 并跳转

## 关键文件

- `drivers/ota_partition.h` — 分区常量
- `drivers/fal/` — FAL + NOR 驱动
- `drivers/qboot_vexpress.c` — Cortex-A 跳转（NOR → RAM）
- `qemu-vexpress-a9-boot/` — QBoot 独立工程
