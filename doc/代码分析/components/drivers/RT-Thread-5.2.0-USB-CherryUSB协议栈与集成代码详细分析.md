# RT-Thread 5.2.0 USB（CherryUSB 协议栈与集成）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/usb` 目录的 **组织方式与实现要点**。该目录在 5.2.0 中 **不再包含旧版独立 `usbdevice`/`usbhost` 源码树**，而是通过 **`Kconfig` 引入、`SConscript` 递归编译** 子目录 **`cherryusb/`**（**[CherryUSB](https://github.com/cherry-embedded/CherryUSB)** 嵌入式 USB 协议栈的 RT-Thread 集成副本）。

**`rtdevice.h`** 仍可按 **`RT_USING_USB_DEVICE` / `RT_USING_USB_HOST`** 包含 **`drivers/usb_device.h`、`drivers/usb_host.h`**（**`components/drivers/include/drivers/`** 下的 **传统 RT-Thread USB 设备/主机 API 头文件**），与 **CherryUSB 的 `usbd_core.h` / `usbh_core.h`** 属于 **不同软件栈**；新工程若 **仅启用 CherryUSB**，应以 **`RT_USING_CHERRYUSB`** 及 **`usb_config.h`** 为准配置，避免混用两套初始化与符号。

---

## 1. 顶层 `usb/` 目录（聚合层）

| 文件 | 作用 |
|------|------|
| **`Kconfig`** | **`rsource "cherryusb/Kconfig"`**，将 **CherryUSB 全部菜单** 挂入内核配置 |
| **`SConscript`** | 遍历当前目录下 **含 `SConscript` 的子目录** 并 **`SConscript(...)`** 合并 **`objs`**（当前实际 **仅有 `cherryusb/`**） |

因此 **`components/drivers/usb`** 本身 **无独立 `.c` 文件**，分析重点在 **`cherryusb/`**。

---

## 2. CherryUSB 目录结构（逻辑分层）

| 目录 | 内容 |
|------|------|
| **`common/`** | **描述符、端点、日志、内存拷贝、DC/HC 抽象头文件** 等（**`usb_def.h`、`usb_dc.h`、`usb_hc.h`、`usb_errno.h`** 等） |
| **`core/`** | **`usbd_core.c/.h`（Device）**、**`usbh_core.c/.h`（Host）** — 枚举、控制传输、事件分发 |
| **`class/`** | **Class 驱动**：**`cdc`/`hid`/`msc`/`audio`/`video`/`dfu`/`hub`/`wireless`/`midi`/`adb`/`vendor/*`** 等 |
| **`port/`** | **芯片 IP 适配**：**Device Controller（`usb_dc_*.c`）**、**Host Controller（`usb_hc_*.c`）** 及 **`usb_glue_*.c`（时钟/PHY/引脚/中断桥接）**；常见 **DWC2、FSDEV、MUSB、EHCI、OHCI、ChipIdea、Kinetis、RP2040、HPM、CH32…** |
| **`osal/`** | **`usb_osal_rtthread.c`**：把 CherryUSB 需要的 **线程/信号量/互斥/延时/临界区** 映射到 **RT-Thread API** |
| **`demo/`** | **设备侧模板工程**（CDC ACM、MSC RAM、HID、RNDIS/ECM/NCM、WinUSB、Audio、Video 等） |
| **`platform/rtthread/`** | 与 **RT-Thread 块设备** 等结合的模板（如 **MSC+块设备**） |

版本信息见 **`cherryusb/VERSION`**；说明文档见 **`README.md` / `README_zh.md`**。

---

## 3. Kconfig 总览（`cherryusb/Kconfig`）

顶层：**`menuconfig RT_USING_CHERRYUSB`**。

### 3.1 设备模式 **`RT_CHERRYUSB_DEVICE`**

- **速度**：**FS / HS / AUTO**（**HS** 时 SConscript 增加 **`CONFIG_USB_HS`**）。
- **Device IP**：**`CUSTOM`、`FSDEV`、`DWC2_*`、`MUSB_*`、`KINETIS_*`、`CHIPIDEA_*`、博流 **`BL`**、**`HPM`、`AIC`、`CH32`、`PUSB2`、`NRF5X`** 等；**`PUSB2`** 等可能链接 **预编译 `.a`**。
- **类驱动开关**：**CDC ACM、HID、MSC、Audio、Video、RNDIS、ECM、NCM、DFU** 等（**各自对应 `class/` 下源文件**）。
- **模板**：**`none` 或各类 `demo/*.c`/`platform/rtthread/*.c`**，用于快速出 **复合设备**。

### 3.2 主机模式 **`RT_CHERRYUSB_HOST`**

- **Host IP**：**EHCI、DWC2、MUSB、OHCI、Kinetis、`PUSB2`、`XHCI`** 等及 **各 `usb_glue_*.c`**。
- **类驱动**：**CDC ACM、HID、MSC**（**`select RT_USING_DFS` + `ELMFAT`**）、**CDC ECM/RNDIS/NCM**（**`select RT_USING_LWIP`**）、**Video/Audio/蓝牙/ASIX 等**（部分标注 **商业授权** 或额外 **CONFIG_USBHOST_PLATFORM_***）。

---

## 4. 编译系统（`cherryusb/SConscript`）要点

- **`CPPPATH`**：累加 **`common`、`core`、`class/...`** 等，保证 **`#include "usb_config.h"`** 等由 **BSP 或 `port/*/rt-thread/usb_config.h`** 提供。
- **Device**：**`RT_CHERRYUSB_DEVICE`** → **`usbd_core.c` + `usb_osal_rtthread.c`** + **所选 `port/` DC 与 glue** + **按 Kconfig 打开的 class 与 template**。
- **Host**：**`RT_CHERRYUSB_HOST`** → **`usbh_core.c` + `usbh_hub.c` + `usb_osal_rtthread.c`** + **所选 HC** + **类驱动**；**`demo/usb_host.c`** 等由模板选项追加。

---

## 5. 核心 API 语义（摘）

### 5.1 设备栈（`usbd_core.h`）

- **`enum usbd_event_type`**：**RESET、SOF、CONNECTED/DISCONNECTED、SUSPEND/RESUME、CONFIGURED、SET_INTERFACE、REMOTE_WAKEUP、INIT/DEINIT** 等。
- **`usbd_initialize(busid, reg_base, event_handler)` / `usbd_deinitialize`**。
- **`usbd_add_interface` / `usbd_add_endpoint`**；描述符注册 **`usbd_desc_register`**（或 **ADVANCE_DESC** 变体）。
- **硬件 IRQ**：在 **`usb_dc_*`** 中 **`USBD_IRQHandler`** 宏已废弃，需在 **向量入口调用栈提供的 IRQ 处理函数**（见头文件 **`#error` 提示**）。

### 5.2 主机栈（`usbh_core.h`）

- **`struct usbh_class_info` + `CLASS_INFO_DEFINE`**：通过 **段属性 `.usbh_class_info`** 收集 **接口 class/subclass/protocol 与 VID/PID 表**，实现 **自动匹配 class driver**。
- **`struct usbh_class_driver`**：**`connect` / `disconnect`**。
- **Hub**：**`usbh_hub.c`** 作为 **根 Hub 与级联** 的基础。

### 5.3 设备控制器抽象（`usb_dc.h`）

**`usb_dc_init/deinit`、`usbd_ep_open/close/...`** 等，由 **具体 `port/*/usb_dc_*.c`** 实现，**`usbd_core`** 通过 **`busid`** 区分多路 USB。

### 5.4 RT-Thread OSAL（`usb_osal_rtthread.c`）

**线程、信号量、互斥量、msleep、临界区** 等与 **CherryUSB 内部调用**对接；创建失败时部分路径 **`while(1)` 死等**（与上游实现一致，产品化时可评估改为 **返回错误**）。

---

## 6. 与 BSP 的集成要点

1. **`usb_config.h`**：定义 **`CONFIG_USB_*`**（端点数量、FIFO、主机通道数、类开关等），通常放在 **板级 `applications` 或 `board/CubeMX` 生成目录`**，并保证 **`-I` 路径** 在 **`cherryusb/SConscript` 的 `path`** 中可见。
2. **`usbd_initialize` / `usbh_initialize`**（主机侧 API 见 **`usbh_core.h` 后部`**）：在 **合适的 `INIT_APP_EXPORT` 或 `main`** 中调用，**`reg_base` 与 `busid`** 与 **硬件手册** 一致。
3. **IRQ**：**`port/*/usb_*glue*.c`** 中注册 **NVIC/PLIC** 等，内部再调 **CherryUSB 提供的 `*_IRQHandler`**。
4. **双栈并存**：若工程仍链接 **旧 `RT_USING_USB_DEVICE` 栈**（若存在于其它软件包），须 **避免 GPIO/DMA/端点资源冲突**；**5.2.0 本仓库 `drivers/usb` 仅 CherryUSB**，旧栈需从 **其它路径** 引入。

---

## 7. 小结

| 层次 | 位置 | 职责 |
|------|------|------|
| **聚合** | **`drivers/usb/*`** | **仅 Kconfig + 递归 SConscript** |
| **协议栈** | **`cherryusb/`** | **Device/Host 核心、类驱动、OSAL、Demo** |
| **SoC 适配** | **`cherryusb/port/`** | **DC/HC + glue** |
| **传统头文件** | **`drivers/include/drivers/usb_*.h`** | **旧 API，可与 CherryUSB 并存于文档层面，但初始化勿混用** |

新平台建议 **直接以 CherryUSB 文档 + 本树 `port` 下相近 SoC 为模板**，通过 **`RT_USING_CHERRYUSB`** 完成 **Device/Host 与类功能** 的裁剪与验证。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/usb/`（实现主体在 `cherryusb/`）。*
