# RT-Thread 5.2.0 — QEMU VExpress-A9 上运行 `rtthread.elf` / `rtthread.bin`

本文对应 BSP：**`rt-thread-5.2.0/bsp/qemu-vexpress-a9`**。在 Ubuntu 下完成 **`scons`** 编译后，会在该目录生成 **`rtthread.elf`**、**`rtthread.bin`**。运行需在**同一 BSP 目录**执行，并依赖 **QEMU**。

---

## 1. 前置条件

- 已安装：**`qemu-system-arm`**  

  ```bash
  sudo apt install -y qemu-system-arm
  ```

- 当前工作目录：**`.../rt-thread-5.2.0/bsp/qemu-vexpress-a9`**（与编译输出 **`rtthread.bin`** 所在位置一致）。

---

## 2. 推荐方式：使用 BSP 自带脚本

| 脚本 | 说明 |
|------|------|
| **`./qemu.sh`** | **`-serial stdio`**：串口输出到**当前终端**；首次运行若无 **`sd.bin`** 会自动 **`dd`** 生成 **64MB** 虚拟 SD |
| **`./qemu-nographic.sh`** | **`-nographic`**：无图形，适合 SSH；同样会准备 **`sd.bin`** |

脚本内实际使用的内核镜像为 **`rtthread.bin`**（与 **`arm-none-eabi-objcopy`** 生成物一致）。

**启动示例：**

```bash
cd /home/work2/RtThreadOsLearn/rt-thread-5.2.0/bsp/qemu-vexpress-a9
./qemu-nographic.sh
```

**退出 QEMU（`-nographic` 常用）：** **`Ctrl+A`**，松开后按 **`X`**（退出 QEMU 监视器命令）。

在 **`./qemu.sh`**（非 `-nographic`）场景下，也可尝试 **`Ctrl+C`** 结束进程（取决于终端对 QEMU 的信号传递）。

---

## 3. 与脚本等价的命令行

### 3.1 无图形（与 `qemu-nographic.sh` 一致）

```bash
cd /home/work2/RtThreadOsLearn/rt-thread-5.2.0/bsp/qemu-vexpress-a9
[ -f sd.bin ] || dd if=/dev/zero of=sd.bin bs=1024 count=65536
qemu-system-arm -M vexpress-a9 -smp cpus=2 -kernel rtthread.bin -nographic -sd sd.bin
```

### 3.2 串口到 stdio（与 `qemu.sh` 一致）

```bash
qemu-system-arm -M vexpress-a9 -smp cpus=2 -kernel rtthread.bin -serial stdio -sd sd.bin
```

**参数含义简述：**

- **`-M vexpress-a9`**：机器类型，需与 BSP 目标一致。  
- **`-smp cpus=2`**：与脚本一致的双核（若需单核可改为 **`cpus=1`**，以 BSP/README 建议为准）。  
- **`-kernel rtthread.bin`**：加载 RT-Thread 镜像。  
- **`-sd sd.bin`**：虚拟 SD；不需要 SD 时可去掉（视文件系统/测试需求而定）。

---

## 4. 使用 `rtthread.elf` 代替 `rtthread.bin`

官方 README 提到 QEMU 可使用 **elf**。可尝试：

```bash
qemu-system-arm -M vexpress-a9 -smp cpus=2 -kernel rtthread.elf -nographic -sd sd.bin
```

若本机 QEMU 报错或无法正确启动，**请改回 `-kernel rtthread.bin`**（与 **`qemu.sh`** / **`qemu-nographic.sh`** 行为一致，最稳妥）。

---

## 5. GDB 调试（`qemu-dbg.sh`）

**`qemu-dbg.sh`** 内容等价于在 **`qemu-nographic.sh`** 基础上增加：

- **`-S`**：启动后暂停 CPU（等待调试器连接）  
- **`-s`**：在默认端口 **1234** 打开 GDB stub  

```bash
./qemu-dbg.sh
```

另一终端用 **arm-none-eabi-gdb**（或 **`gdb-multiarch`**）加载 **`rtthread.elf`** 后执行 **`target remote :1234`** 即可调试（具体 GDB 命令以你本机工具链为准）。

---

## 6. 与编译文档的关系

- 编译环境：**`doc/RT-Thread-5.2.0-Ubuntu-编译环境搭建.md`**  
- BSP 更完整说明：**`rt-thread-5.2.0/bsp/qemu-vexpress-a9/README.md`**

---

*文档路径：`doc/RT-Thread-5.2.0-qemu-vexpress-a9-运行说明.md`*
