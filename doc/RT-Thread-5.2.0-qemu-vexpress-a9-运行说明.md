# RT-Thread 5.2.0 — QEMU VExpress-A9 运行说明

对应 BSP：**`rt-thread-5.2.0/bsp/qemu-vexpress-a9`**（App）与 **`qemu-vexpress-a9-boot`**（QBoot）。在 Ubuntu 下用 **`scons`** 编译后，在 **`qemu-vexpress-a9`** 目录运行 QEMU。

---

## 1. 前置条件

```bash
sudo apt install -y qemu-system-arm gcc-arm-none-eabi
```

工作目录通常为：**`.../rt-thread-5.2.0/bsp/qemu-vexpress-a9`**。

---

## 2. 两种启动方式

| 脚本 | 用途 |
|------|------|
| **`./qemu-app-only.sh`** | 仅启动 App（`-kernel rtthread.elf`），调试 App 最快 |
| **`./qemu-nographic.sh`** | QBoot 链式启动：NOR 中 QBoot → 从 `app` 分区加载到 RAM → 跳转 App |

| 脚本 | 说明 |
|------|------|
| **`./qemu.sh`** | 与 `qemu-nographic.sh` 同类，但使用 **`-serial stdio`**（有图形终端时） |

**退出 QEMU（`-nographic`）：** **`Ctrl+A`**，松开后按 **`X`**。MSH 里没有 `exit` 命令。

---

## 3. QBoot 链式启动（推荐验证 OTA 布局）

### 3.1 编译与烧写 flash 镜像

每次修改 **App** 或 **Boot** 后，需重新编译并生成 **`flash0.bin`**：

```bash
# 1. 编译 App
cd rt-thread-5.2.0/bsp/qemu-vexpress-a9
scons -c && scons

# 2. 编译 Boot
cd ../qemu-vexpress-a9-boot
scons -c && scons

# 3. 生成 64MB flash0.bin（qboot @ 0，app @ 512KB），并复制到 app 目录
./flash_image.sh

# 4. 启动
cd ../qemu-vexpress-a9
./qemu-nographic.sh
```

**说明：** `flash_image.sh` 会写入 **`qemu-vexpress-a9-boot/flash0.bin`** 并同步到 **`qemu-vexpress-a9/flash0.bin`**。若只跑 Boot 目录下的脚本而未同步，App 分区可能为空，链式启动会失败。

### 3.2 正常日志含义

1. 先出现 **QBoot** 版本信息（`Qboot startup ...`）。
2. **`[E/Qboot] partition "download" infomation check fail`**：download 分区尚无合法 OTA 包，**可忽略**；QBoot 会继续从 **`app`** 分区启动。
3. **`QBoot: jump to application @ 0x600717b8 ...`**：从 NOR 拷贝 App 到 RAM `0x60010000` 并跳转。
4. 再次出现 RT-Thread 横幅、`Hello RT-Thread!`、`msh />` 及 `main.c` 多线程 demo。

### 3.3 NOR 分区布局（FAL）

| 分区 | 偏移 | 大小 |
|------|------|------|
| bl | 0 | 512KB |
| app | 512KB | 4MB |
| download | 4.5MB | 4MB（OTA 暂存） |
| param | 8.5MB | 512KB |

---

## 4. 仅启动 App（开发调试）

```bash
cd rt-thread-5.2.0/bsp/qemu-vexpress-a9
scons
./qemu-app-only.sh
```

使用 **`rtthread.elf`** 加载到链接地址 **`0x60010000`**，**`-smp 1`**（与当前非 SMP 配置一致）。可选挂载 **`flash0.bin`**（若存在）以测试 FAL。

---

## 5. 与脚本等价的命令行（App 直启）

```bash
cd rt-thread-5.2.0/bsp/qemu-vexpress-a9
[ -f sd.bin ] || dd if=/dev/zero of=sd.bin bs=1024 count=65536
qemu-system-arm -M vexpress-a9 -smp 1 \
    -kernel rtthread.elf \
    -drive if=pflash,format=raw,file=flash0.bin \
    -drive if=sd,format=raw,file=sd.bin \
    -nographic
```

---

## 6. GDB 调试（`qemu-dbg.sh`）

在 **`qemu-nographic.sh`** 基础上增加 **`-S -s`**（CPU 暂停，GDB stub 端口 **1234**）：

```bash
./qemu-dbg.sh
```

另一终端：`gdb-multiarch rtthread.elf` → **`target remote :1234`**。

---

## 7. 相关文档

- 编译环境：**`doc/RT-Thread-5.2.0-Ubuntu-编译环境搭建.md`**
- BSP README：**`rt-thread-5.2.0/bsp/qemu-vexpress-a9/README.md`**
- OTA 说明：**`rt-thread-5.2.0/bsp/qemu-vexpress-a9/README_OTA.md`**

---

*文档路径：`doc/RT-Thread-5.2.0-qemu-vexpress-a9-运行说明.md`*
