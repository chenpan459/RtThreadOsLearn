# RT-Thread 5.2.0 在 Ubuntu 下编译环境搭建

本文说明在 **Ubuntu** 上使用 **GCC + SCons** 编译本仓库 **`rt-thread-5.2.0`** 的推荐步骤，与多数 BSP 的 **README** 一致；具体 BSP 若另有工具链要求，以该 **`bsp/<board>/README.md`** 与 **`rtconfig.py`** 为准。

---

## 1. 安装系统软件包

```bash
sudo apt update
sudo apt install -y \
  python3 python3-pip python3-venv \
  scons build-essential git \
  libncurses-dev pkg-config \
  gcc-arm-none-eabi libnewlib-arm-none-eabi
```

| 包 | 作用 |
|----|------|
| **python3 / pip** | SCons 与 Kconfig 前端依赖 Python |
| **scons** | RT-Thread 默认构建系统 |
| **build-essential** | 基础编译工具（make 等，部分脚本会用到） |
| **git** | 克隆或更新源码 |
| **libncurses-dev** | **`scons --menuconfig`** 终端菜单界面 |
| **pkg-config** | 部分 BSP/包可能依赖 |
| **gcc-arm-none-eabi / libnewlib-arm-none-eabi** | 常见 **ARM Cortex-M** 与 **部分 Cortex-A BSP**（如 **`bsp/qemu-vexpress-a9`** 使用 **`arm-none-eabi-`** 前缀） |

**说明**：若目标 BSP 使用 **RISC-V、AArch64 Linux 工具链** 等，需按 **`rtconfig.py`** 中 **`PREFIX`/`EXEC_PATH`** 额外安装对应 **`apt`** 包或厂商工具链，本文不逐一列举。

---

## 2. 安装 Python 构建依赖（含 menuconfig）

在 **RT-Thread 源码根目录**（例如 **`/home/work2/RtThreadOsLearn/rt-thread-5.2.0`**）执行：

```bash
cd /home/work2/RtThreadOsLearn/rt-thread-5.2.0
pip3 install --user -r tools/requirements.txt
```

**`tools/requirements.txt`** 主要包含：

- **scons**（若仅用 apt 的 scons，版本需满足 **`>=4.0.1`**）
- **kconfiglib**（**`scons --menuconfig`** 必需）
- **requests、tqdm、PyYAML**（工具脚本/包管理可能使用）

若 **`pip3 install --user`** 的可执行目录不在 **`PATH`** 中，可将类似以下内容加入 **`~/.bashrc`**：

```bash
export PATH="$HOME/.local/bin:$PATH"
```

---

## 3. 交叉编译器路径（可选）

多数 BSP 默认 **`EXEC_PATH = '/usr/bin'`**，系统 **`arm-none-eabi-gcc`** 在 **`PATH`** 中即可。

若工具链安装在自定义目录，可参考 **`bsp/qemu-vexpress-a9/rtconfig.py`** 中的环境变量：

```bash
export RTT_EXEC_PATH=/path/to/gcc-arm-none-eabi/bin
export RTT_CC_PREFIX=arm-none-eabi-
```

具体变量名以 **当前 BSP 的 `rtconfig.py`** 为准。

---

## 4. 编译流程（必须在 BSP 目录内）

RT-Thread **不能**在仓库根目录脱离 BSP 直接完成完整固件编译；需进入 **某一 BSP**：

```bash
cd /home/work2/RtThreadOsLearn/rt-thread-5.2.0/bsp/qemu-vexpress-a9
```

将路径替换为你的目标板 **`bsp/<厂商>/<板名>`**。

### 4.1 配置（可选）

```bash
scons --menuconfig
```

保存退出后，一般会更新 **`rtconfig.h`**（及个别 BSP 的派生配置）。

### 4.2 编译

```bash
scons -j$(nproc)
```

成功后通常在 BSP 目录下生成 **`rtthread.elf`**、**`rtthread.bin`** 等（以该 BSP **`SConstruct`** 为准）。

---

## 5. QEMU 仿真（可选）

若使用 **`bsp/qemu-vexpress-a9`** 等 QEMU BSP，可安装：

```bash
sudo apt install -y qemu-system-arm
```

在该 BSP 目录下使用自带的 **`qemu.sh`**、**`qemu-nographic.sh`** 等脚本运行（见 BSP **README**）。

---

## 6. 常见问题

| 现象 | 处理 |
|------|------|
| **`scons: command not found`** | `sudo apt install scons` 或 `pip3 install --user 'scons>=4.0.1'` |
| **menuconfig 与 kconfig 相关报错** | 在源码根执行 **`pip3 install --user -r tools/requirements.txt`**，并检查 **`PATH`** 含 **`~/.local/bin`** |
| **`arm-none-eabi-gcc: not found`** | 安装 **`gcc-arm-none-eabi`**，或设置 **`RTT_EXEC_PATH`** |
| **某 BSP 编译报工具链前缀错误** | 打开该 BSP **`rtconfig.py`**，按 **`PREFIX`** 安装对应工具链 |

---

## 7. 与 Windows ENV 的关系

官方常提供 **RT-Thread ENV（Windows）** 一键环境。在 **Ubuntu** 上等价做法是：**apt + pip 依赖 + 正确交叉 GCC + 进入 BSP 执行 scons**，不必强制使用 Windows ENV。

---

## 8. 相关文档

- 仓库内 **`README_zh.md`**、**`documentation/`**  
- 各 **`bsp/<board>/README.md`**  
- 本仓库学习笔记目录下的模块分析：**`doc/RT-Thread-5.2.0-*.md`**

---

*文档路径：`doc/RT-Thread-5.2.0-Ubuntu-编译环境搭建.md`*  
*对应源码版本：RT-Thread **5.2.0**（路径以 **`/home/work2/RtThreadOsLearn/rt-thread-5.2.0`** 为例，可按本机实际修改）。*
