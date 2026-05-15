# RT-Thread 5.2.0 VirtIO（MMIO 虚拟设备驱动）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/virtio` 目录实现的 **VirtIO 1.0 MMIO 传输层与若干类驱动**（**BLK / NET / CONSOLE / GPU / INPUT**），面向 **QEMU virt 等虚拟机或半虚拟化环境**：通过 **`struct virtio_mmio_config`** 访问 **Magic/Version/DeviceID/队列/中断/状态位**，用 **split virtqueue（desc/avail/used）** 与 **Hypervisor** 交换数据。

**说明**：**`rtdevice.h` 不包含 `virtio.h`**，BSP 需 **`#include <virtio.h>`** 及 **`virtio_blk.h` 等**；**`components/drivers/Kconfig`** 通过 **`rsource "virtio/Kconfig"`** 挂入 **`RT_USING_VIRTIO`** 及子选项。

---

## 1. 目录与编译

| 文件类型 | 文件 | 作用 |
|----------|------|------|
| **公共** | **`virtio.h`、`virtio_mmio.h`、`virtio_queue.h`** | **设备抽象、MMIO 寄存器布局、virtqueue 描述符与环** |
| **核心** | **`virtio.c`** | **复位/状态/中断应答、队列分配与描述符链、`virtio_submit_chain`、（可选）VirtIO GPU 与 framebuffer 的 SMART/SDL 辅助代码** |
| **类驱动** | **`virtio_blk.c`、`virtio_net.c`、`virtio_console.c`、`virtio_gpu.c`、`virtio_input.c`** | 各 **`rt_virtio_*_init(mmio_base, irq)`** 与 **ISR** |
| **构建** | **`Kconfig`、`SConscript`** | **`RT_USING_VIRTIO`** 下 **`Glob('*.c')`**，**`CPPPATH`** 为当前目录 |

---

## 2. Kconfig 摘要

| 选项 | 含义 |
|------|------|
| **`RT_USING_VIRTIO`** | 总开关 |
| **`RT_USING_VIRTIO10`** | **VirtIO v1.0**（**`virtio.h` 中 `RT_USING_VIRTIO_VERSION` = 0x1**） |
| **`RT_USING_VIRTIO_MMIO_ALIGN`** | **`struct virtio_mmio_config` 使用 `packed`**，避免编译器对 MMIO 的过度优化（见头文件注释） |
| **`RT_USING_VIRTIO_BLK` / `NET` / `CONSOLE` / `GPU` / `INPUT`** | 各子 **`.c` 用 `#ifdef` 包裹**；**Console** 下 **`RT_USING_VIRTIO_CONSOLE_PORT_MAX_NR`**（默认 **4**） |

---

## 3. 公共头文件与约束

### 3.1 `virtio.h`

- **`RT_NAME_MAX < 16` 时 `#error`**：设备命名依赖较长字符串。
- **`RT_USING_SMART`**：**`VIRTIO_VA2PA` / `VIRTIO_PA2VA`** 通过 **`rt_kmem_v2p` / `rt_ioremap`**；否则 **恒等映射**。
- **`struct virtio_device`**：**`irq`、`queues`/`queues_num`、`mmio_base` 或 `mmio_config` 指针联合体**；**SMP** 下 **`spinlock`**。
- **设备 ID 枚举**：与规范一致的 **`VIRTIO_DEVICE_ID_*`**（本树主要使用 **BLOCK/NET/CONSOLE/GPU/INPUT**）。
- **对外过程**：**`virtio_reset_device`、`virtio_status_*`、`virtio_interrupt_ack`、`virtio_has_feature`**；**队列**：**`virtio_queues_alloc/free`、`virtio_queue_init/destroy`、`virtio_queue_notify`、`virtio_submit_chain`**；**描述符**：**`virtio_alloc_desc`、`virtio_free_desc`、`virtio_alloc_desc_chain`、`virtio_free_desc_chain`、`virtio_fill_desc`**。

### 3.2 `virtio_mmio.h`

**`struct virtio_mmio_config`** 覆盖 **0x00–0x100+ config 空间**，字段布局与 **VirtIO 1.0 MMIO** 规范一致（**Magic `0x74726976`、device_features、queue_sel、interrupt_status/ack、status、queue 地址高/低 32 位等**）。

### 3.3 `virtio_queue.h`

**经典 split queue**：**`virtq_desc` / `virtq_avail` / `virtq_used`** 及 **`struct virtq` 辅助域**（**`used_idx`、`free[]`、`free_count`**）。**宏 `VIRTQ_*_TOTAL_SIZE`** 用于 **`virtio_queue_init` 中连续内存布局**。

---

## 4. `virtio.c`：队列与描述符

### 4.1 `virtio_queue_init`

- **`rt_malloc_align(..., VIRTIO_PAGE_SIZE)`** 分配 **desc + avail + used** 连续区域，**按页对齐 used 环**。
- 写 MMIO：**`guest_page_size`、`queue_sel`、`queue_num`、`queue_align`、`queue_pfn = PA >> 12`**（**依赖 Guest 物理地址与 Host 可见内存一致**；VirtIO 1.0 另有 **queue_desc/driver/device 64 位寄存器**，本实现以 **`queue_pfn` 兼容路径** 为主，适用于 **常见 QEMU virt 环境**）。
- 初始化 **`free[]`** 与 **`free_count`**。

### 4.2 `virtio_submit_chain`

- **`avail->ring[idx % ring_size] = desc_index`**，**`avail->idx++`**，中间 **`rt_hw_dsb()`** 保证 **设备可见顺序**。

### 4.3 描述符分配

- **线性扫描 `free[]`**，无 **独立空闲链表**；**SMP 下由各类驱动自旋锁保护**（如 **BLK** 在 **`virtio_blk_rw` 全程持锁**）。

### 4.4 GPU 相关（**`#if RT_USING_SMART && RT_USING_VIRTIO_GPU`**）

在同文件后部提供 **framebuffer 设备与 `virtio_gpu` 协同** 的 **POSIX fb ioctl / mmap** 等 glue（**支持 SDL2** 相关 ChangeLog），与 **`virtio_gpu.c`** 主逻辑配合；仅 **SMART + GPU** 配置时参与编译。

---

## 5. 类驱动概要

### 5.1 BLK（`virtio_blk.c`）

- **`rt_virtio_blk_init`**：**`virtio_reset_device` → acknowledge → 写 `driver_features`（关闭 RO/MQ/SCSI 等未实现位）→ `virtio_status_driver_ok`**；**单队列 `VIRTIO_BLK_QUEUE_RING_SIZE=4`**。
- **`virtio_blk_rw`**：构造 **3 段描述符**（**req、data、status**），**`notify` 后轮询 `info[].valid` 直至 ISR 清除**（**同步阻塞 I/O**）。
- **注册**：**`RT_Device_Class_Block`**，**`rt_hw_interrupt_install`** 安装 **`virtio_blk_isr`**（**`virtio_interrupt_ack` + 扫描 `used` ring**）。
- **几何**：**`GETGEOME`** 中 **`bytes_per_sector` 固定 512**，**`block_size`/`sector_count` 来自 `virtio_blk_config`**。

### 5.2 NET（`virtio_net.c` + `virtio_net.h`）

- **双队列 RX=0、TX=1**，**`struct eth_device`** 接入 **LwIP `ethernetif`**。
- **特性位、MAC、`virtio_net_hdr`、config 结构** 与规范对齐；**`rt_virtio_net_init`** 完成 **特征协商、队列、eth 注册与中断**（细节见源码）。

### 5.3 Console（`virtio_console.c` + `virtio_console.h`）

- **多端口**：**`VIRTIO_CONSOLE_PORT_QUEUE_INDEX`** 映射 **每端口 RX/TX**；**控制队列** 与 **`virtio_console_control` 事件**（**DEVICE_READY、PORT_ADD、RESIZE** 等）。
- **`RT_USING_VIRTIO_CONSOLE_PORT_MAX_NR`** 控制 **端口数量上限**。

### 5.4 GPU（`virtio_gpu.c` + `virtio_gpu.h`）

- **显示资源创建、扫描输出、2D 填充/复制** 等 **VirtGPU 协议** 侧实现；**`rt_virtio_gpu_init`** 注册 **图形设备** 并与 **`virtio.c` 中 fb 层**（若启用）衔接。

### 5.5 Input（`virtio_input.c` + `virtio_input.h`、`virtio_input_event_codes.h`）

- **事件队列 + 配置空间** 解析；**事件码** 与 **Linux evdev 风格** 对齐（**`virtio_input_event_codes.h`**），供 **GUI/输入子系统** 消费。

---

## 6. BSP 集成模式（以 QEMU virt 为例）

**`bsp/qemu-virt64-aarch64/drivers/drv_virtio.c`**（RISC-V 变体类似）：

1. **`VIRTIO_MMIO_BASE` / `VIRTIO_MMIO_SIZE` / `VIRTIO_MAX_NR` / `VIRTIO_IRQ_BASE`** 等板级常量；**`VIRTIO_VENDOR_ID`** 亦由 **BSP 定义**（如 QEMU virt 常为 **`0x554d4551`「QEMU」**，见 **`bsp/qemu-virt64-aarch64/drivers/virt.h`**）。
2. **`rt_ioremap`** 映射 **MMIO 窗口**。
3. **循环每个槽位**：检查 **`magic == VIRTIO_MAGIC_VALUE`、`version == RT_USING_VIRTIO_VERSION`、`vendor_id == VIRTIO_VENDOR_ID`**。
4. 以 **`device_id` 为下标** 查 **`virtio_device_init_handlers[]`**，非空则 **`init_handler((rt_ubase_t *)mmio_base, irq)`**。
5. **`INIT_DEVICE_EXPORT(rt_virtio_devices_init)`** 自动枚举。

**`BSP_USING_VIRTIO_*`** 与 **`RT_USING_VIRTIO_*`** 需 **在配置中同时满足** 才会链接对应驱动。

---

## 7. 使用与移植注意

1. **内存与缓存**：缓冲区地址经 **`VIRTIO_VA2PA`** 写入 **描述符**；**带 DCache 的 CPU** 需在 **合适位置 clean/invalidate**（**NET/GPU** 等路径尤需注意）。
2. **性能**：当前 **BLK 为忙等完成**；高吞吐场景可评估 **异步 + 信号量/完成队列** 改造。
3. **版本**：Kconfig 当前 **choice 仅列出 v1.0**；若未来扩展 **非 1.0**，需同步 **`drv_virtio.c` 的版本比较** 与 **队列寄存器路径**。
4. **头文件包含顺序**：依赖 **`cpuport.h`/`rtdevice.h`** 的 BSP 请保证 **与 `virtio.h` 不循环包含**。

---

## 8. 小结

| 层次 | 位置 | 职责 |
|------|------|------|
| **传输与队列** | **`virtio.c` + `virtio_queue.h`** | **MMIO 访问、virtqueue 生命周期、描述符链提交** |
| **类设备** | **`virtio_*.c`** | **BLK/NET/Console/GPU/Input 协议与 `rt_device`/`eth_device` 注册** |
| **枚举与映射** | **BSP `drv_virtio.c`** | **MMIO 槽位扫描、`device_id → init` 分发、IRQ 号** |

该目录是 RT-Thread 在 **虚拟化平台** 上获得 **磁盘、网络、控制台与图形输入** 的 **主路径之一**；物理机 **非 VirtIO** 场景无需编译本组。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/virtio/`。*
