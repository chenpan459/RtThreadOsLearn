# RT-Thread 5.2.0 `components/legacy` 模块详细分析

**`legacy`** 目录用于在 **5.x** 主线中保留 **旧版 API 与旧版子系统**，避免老 BSP / 老工程因接口或 USB/FDT 栈切换而大面积失效。总开关为 **`RT_USING_LEGACY`**（`components/Kconfig` 中 **`Support legacy version for compatibility`**，默认 **n**）。

---

## 1. 编译与目录总览

**`components/legacy/SConscript`** 行为：

1. 将 **`ipc/workqueue_legacy.c`** 编入 **`Legacy`** 组，依赖 **`RT_USING_LEGACY`**。  
2. 若 **`RT_USING_DFS`**：把 **`legacy/dfs`** 加入 **`CPPPATH`**（该目录下为 **头文件兼容层**，无对应 `.c` 编入此脚本）。  
3. 遍历 **`legacy`** 下带 **`SConscript`** 的一级子目录并合并（当前为 **`usb`**、**`fdt`**）。

**`components/legacy/Kconfig`** 仅 **`rsource`**：

- **`usb/Kconfig`** — 旧版 USB Host/Device  
- **`fdt/Kconfig`** — 旧版 FDT 封装（与 **`drivers/ofw`** 所用 libfdt 可能并存，需注意符号/头文件冲突）

根 **`components/Kconfig`** 在 **`endmenu` 前** 无条件 **`rsource "legacy/Kconfig"`**，因此 USB/FDT 菜单项始终出现在配置树中；是否编译进镜像仍由各自 **`GetDepend`** 与 **`RT_USING_LEGACY`** 组合决定。

---

## 2. `rtlegacy.h` — 全局兼容宏

**`rtlegacy.h`** 提供 **5.0 以前** 常用的版本号与属性宏别名，例如：

- **`RT_VERSION` / `RT_SUBVERSION` / `RT_REVISION`** → 映射到 **`RT_VERSION_MAJOR/MINOR/PATCH`**  
- **`RT_SECTION` / `RT_WEAK` / `RT_USED` / `ALIGN`** → 映射到 **`rt_section` / `rt_weak` / `rt_used` / `rt_align`**

在 **`RT_USING_DEVICE_IPC`** 时包含 **`ipc/workqueue_legacy.h`**，以保留 **`struct rt_delayed_work`** 与 **`rt_delayed_work_init`** 的旧式工作队列接口（见下节）。

---

## 3. `ipc/workqueue_legacy` — 延迟工作队列兼容

| 文件 | 作用 |
|------|------|
| **`workqueue_legacy.h`** | 定义 **`struct rt_delayed_work`**（内嵌 **`struct rt_work work`**），声明 **`rt_delayed_work_init`** |
| **`workqueue_legacy.c`** | **`rt_delayed_work_init`** 内部调用 **`rt_work_init`**，把旧 API 接到新 **`ipc/workqueue`** 实现 |

老代码若仍使用 **`rt_delayed_work_*`**，在开启 **`RT_USING_LEGACY`** 后可继续链接；头文件通过 **`rtlegacy.h`** 在 **`RT_USING_DEVICE_IPC`** 条件下引入。

---

## 4. `dfs/` — DFS 相关头文件兼容

目录内为 **`dfs_posix.h`、`dfs_select.h`、`dfs_poll.h`** 等，用于历史工程对 **POSIX select/poll** 与 DFS 声明路径的兼容。

**`SConscript`** 仅在 **`RT_USING_DFS`** 时把 **`legacy/dfs`** 加入 **`CPPPATH`**，不单独增加一组 DFS 源文件；实质是 **编译期头文件搜索路径** 补充。

---

## 5. `usb/` — 旧版 USB 协议栈

与 **`components/drivers/usb/cherryusb`** 等新一代实现并列存在；**老 BSP 仍可选用本栈**。

### 5.1 构建方式

**`legacy/usb/SConscript`** 递归 **`usbhost`**、**`usbdevice`** 子目录。

### 5.2 USB Host（`usb/usbhost/`）

**依赖**：**`RT_USING_USB_HOST`**（并 **`select RT_USING_USB`**）。

**核心源文件**（见 **`usbhost/SConscript`**）：`usbhost_core.c`、`driver.c`、`usbhost.c`、`hub.c`。

**可选类驱动**（由 Kconfig 宏打开）：

| 宏 | 类驱动 | 说明 |
|----|--------|------|
| **`RT_USBH_MSTORAGE`** | `mass.c`、`udisk.c` | U 盘大容量存储；可配 **`UDISK_MOUNTPOINT`** |
| **`RT_USBH_HID`** | `hid.c` | HID 基础 |
| **`RT_USBH_HID_MOUSE`** | `umouse.c` | 鼠标 |
| **`RT_USBH_HID_KEYBOARD`** | `ukbd.c` | 键盘 |
| **`RT_USBH_ADK`** | `adk.c`、`adkapp.c` | Android Open Accessory 等 |

**`CPPPATH`** 含本目录 **`include`** 及向上引用的 **`components/include`**，与 BSP 中 **HCD 移植层** 配合。

### 5.3 USB Device（`usb/usbdevice/`）

**依赖**：**`RT_USING_USB_DEVICE`**。

**核心**：`usbdevice_core.c`、`usbdevice.c`。

**可选类**（节选）：

- **`RT_USB_DEVICE_CDC`**：`cdc_vcom.c`（虚拟串口，可配 RX 缓冲、DMA 等）  
- **`RT_USB_DEVICE_MSTORAGE`**：`mstorage.c`  
- **`RT_USB_DEVICE_HID`**：`hid.c`  
- **`RT_USB_DEVICE_RNDIS` / `RT_USB_DEVICE_ECM`**：`rndis.c`、`ecm.c`（依赖 **`RT_USING_LWIP`**）  
- **`RT_USB_DEVICE_WINUSB`**：`winusb.c`  
- **音频**：`audio_mic.c`、`audio_speaker.c`（由 **`RT_USB_DEVICE_AUDIO_*`** 等宏控制，与 Kconfig 中 composite 选项对应）

支持 **复合设备**（**`RT_USB_DEVICE_COMPOSITE`**）下多类同时打开；VID/PID、USB 线程栈 **`RT_USBD_THREAD_STACK_SZ`** 等在 **`usb/Kconfig`** 中配置。

### 5.4 选型建议

- **新设计**：优先考虑 **`drivers/usb/cherryusb`** 与芯片厂商维护的 port。  
- **维护老工程**：保留 **`RT_USING_LEGACY` + RT_USING_USB_***，BSP 继续对接旧 **Host/Device 核心** 的 HAL 回调。

---

## 6. `fdt/` — 设备树（FDT）旧版封装

### 6.1 Kconfig（`legacy/fdt/Kconfig`）

| 选项 | 含义 |
|------|------|
| **`RT_USING_FDT`** | 启用本 legacy FDT 组件 |
| **`RT_USING_FDTLIB`** | 使用内置 **libfdt**（默认 y） |
| **`RT_USING_FDT_FWNODE`** | 与 **fwnode** 设备模型结合（默认 n） |
| **`FDT_USING_DEBUG`** | 调试输出 |

**`fdt/SConscript`**：仅当 **`GetDepend('RT_USING_FDT')`** 为真时，才递归编译 **`libfdt`**、**`src`**、**`examples`** 等子目录。

### 6.2 目录结构（摘自 `fdt/README.md`）

| 路径 | 说明 |
|------|------|
| **`fdt/libfdt/`** | 上游 **libfdt** 只读/读写 API（`fdt.c`、`fdt_ro.c`、`fdt_rw.c` 等） |
| **`fdt/src/`** | **`dtb_*.c`**：加载 DTB、基址/访问、节点树构建等封装 |
| **`fdt/inc/`** | **`dtb_node.h`、`dtb_fwnode.h`** 等 |
| **`fdt/examples/`** | 示例（如 **`fdt_test.c`**） |
| **`fdt/docs/`** | **`api.md`、`examples.md`** 等 |

### 6.3 许可证与冲突说明

**`fdt/README.md`** 写明本包基于 **libfdt** 封装，并注明 **GPL-3.0** 等许可要求；若工程同时启用 **`drivers/ofw`** 等其它 **libfdt** 来源，需按文档处理 **重复符号/头文件** 问题（README 建议取消重复的 libfdt 包选项）。

### 6.4 与 `drivers/ofw` 的关系

- **`drivers/ofw`**：内核/驱动模型侧 **OFW（Open Firmware）** 与设备树绑定，偏 **DM + 驱动 probe**。  
- **`legacy/fdt`**：偏 **工具型** 在内存/文件加载与修改 DTB、生成节点树；老文档中曾以 **online package** 形式描述。

二者可并存于不同用途；新平台若已全面使用 **`RT_USING_OFW` + DM`**，应评估是否仍需 **`RT_USING_FDT`**。

---

## 7. 配置依赖关系小结

```text
RT_USING_LEGACY
  ├── workqueue_legacy.c +（若 RT_USING_DFS）legacy/dfs 的 CPPPATH
  └── 递归 usb/：各子组另需 RT_USING_USB_HOST 或 RT_USING_USB_DEVICE

RT_USING_FDT（与上并列，由 legacy/fdt/SConscript 判断）
  └── 仅当为 y 时编译 fdt/libfdt、fdt/src、examples 等
```

**说明**：

- **`workqueue_legacy`** 与 **`legacy/dfs` 头路径** 挂在 **`DefineGroup(..., depend=['RT_USING_LEGACY'])`** 上，需 **`RT_USING_LEGACY`**。  
- **旧 USB**：除 **`RT_USING_LEGACY`** 外，还须 **`RT_USING_USB_HOST`** 或 **`RT_USING_USB_DEVICE`**（及 **`RT_USING_USB`**），否则对应 **`rt_usbh`/`rt_usbd`** 组不会编入。  
- **FDT**：**`legacy/fdt/SConscript`** 仅以 **`GetDepend('RT_USING_FDT')`** 为准；源码虽在 **`legacy/fdt`** 下，**理论上可只开 `RT_USING_FDT` 编 FDT**（与是否勾选 **`RT_USING_LEGACY`** 解耦）。实际 BSP 若把整个 **Legacy** 当作一体维护，常 **Legacy + FDT** 一起开，避免只开其一导致头文件或初始化顺序遗漏。

---

## 8. `README.md`

根目录 **`README.md`** 目前仅标题 **「RT-Thread Legacy」**，详细说明以本文档与 **`fdt/README.md`、`fdt/docs/`** 为准。

---

## 9. 相关文档

- 新版 USB：**`components/drivers/usb/cherryusb/README.md`**  
- 设备树与驱动：**`doc/RT-Thread-5.2.0-drivers-模块详细分析.md`**（`ofw` 一节）  
- 组件总览：**`doc/RT-Thread-5.2.0-components-模块详解.md`**

---

*文档对应源码树：`rt-thread-5.2.0/components/legacy`（5.2.0）。*
