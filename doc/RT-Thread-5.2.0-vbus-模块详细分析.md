# RT-Thread 5.2.0 `components/vbus` 模块详细分析

**VBus（Virtual Bus，虚拟总线）** 是 RT-Thread 上的一套**跨操作系统/跨虚拟机**的**软件总线**：在**共享内存**中放置**双向环形缓冲区（ring）**，通过**虚拟中断（virq）**通知对端，在之上建立**多路逻辑通道（channel）**，传输**分块数据包**；并可把 **Finsh/MSH**、**POSIX 文件访问** 等封装成 **字符设备** 挂到 VBus 上，供另一侧（如 **Linux** 或另一 RT-Thread 实例）使用。

本目录代码与 **虚拟化 / AMP / 双系统** 场景强相关；**普通单核裸机 MCU 工程通常不启用**。

---

## 1. 编译与入口

**`components/vbus/SConscript`**：

- 若 **`!GetDepend('RT_USING_VBUS')`**：直接返回空组。  
- **`src = Glob('*.c')`**：根目录下 **`vbus.c`、`vbus_chnx.c`、`prio_queue.c`、`watermark_queue.c`** 等。  
- 若 **`RT_USING_VBUS_RFS`**：追加 **`Glob('utilities/rfs.c')`**。  
- 若 **`RT_USING_VBUS_RSHELL`**：追加 **`Glob('utilities/rshell.c')`**。  

**`CPPPATH`**：**本目录** + **`share_hdr/`**（对外 **`vbus_api.h`** 等）。

**说明**：当前 **`5.2.0` 仓库快照** 下 **`components/vbus/utilities/`** 目录**不存在**，若打开 **`RT_USING_VBUS_RFS`/`RSHELL`** 而文件未由 BSP/软件包提供，**构建会因找不到源文件失败**；需从完整发行版或移植包补齐 **`rfs.c`/`rshell.c`**，或关闭对应选项。

**`DefineGroup('VBus', ..., depend=['RT_USING_VBUS'])`**。

---

## 2. Kconfig（`components/vbus/Kconfig`）

| 选项 | 含义 |
|------|------|
| **`RT_USING_VBUS`** | 总开关（**`menuconfig`**，默认 n） |
| **`RT_USING_VBUS_RFS`** | 远程文件系统（**RFS**），对端通过 VBus 用 **POSIX 文件 I/O** 访问本侧文件 |
| **`RT_USING_VBUS_RSHELL`** | 远程 Shell，对端操作本侧 **Finsh/msh** |
| **`RT_VBUS_USING_TESTS`** | 内置测试 |
| **`_RT_VBUS_RING_BASE`** | **Ring 在物理地址空间的基址**（与 hypervisor/另一 OS 约定一致） |
| **`_RT_VBUS_RING_SZ`** | **Ring 区域总大小**（与 **`RT_VMM_RB_BLK_NR`** 推导相关，见 **`vbus_api.h`**） |
| **`RT_VBUS_GUEST_VIRQ` / `RT_VBUS_HOST_VIRQ`** | **Guest/Host 通知用虚拟中断号**（平台相关） |
| **`RT_VBUS_SHELL_DEV_NAME`** | 远程 Shell 侧 **字符设备名**（默认 **`vbser0`**） |
| **`RT_VBUS_RFS_DEV_NAME`** | **RFS 设备名**（默认 **`rfs`**） |

Ring 与 virq 必须与**对端镜像/设备树/虚拟机配置**一致，否则无法建链。

---

## 3. 协议与共享内存布局（`share_hdr/vbus_api.h`）

- **`struct rt_vbus_ring`**：**`put_idx`/`get_idx`**、**`blocked`**、**`blks[]`** 环形块数组。  
- **`struct rt_vbus_blk`**：固定 **64 字节**一包（含 **4 字节头** + **60 字节 payload**），**`packed`**。  
- **`RT_VBUS_CHANNEL_NR`**：逻辑通道数（默认 **32**）。  
- **`RT_VMM_RB_BLK_NR`**：由 **`_RT_VBUS_RING_SZ / 64 - 1`** 推导，即 ring 容量与配置大小绑定。  
- **通道 0**：**控制面**，命令字 **`ENABLE`/`DISABLE`/`SET`/`ACK`/`NAK`/`SUSPEND`/`RESUME`**（见 **`enum` 与 `vbus.c` 中 `rt_vbus_cmd2str`**）。  
- **`RT_VBUS_USING_FLOW_CONTROL`**：启用**流控与水印**（**`watermark_queue`**）。

对端（如 Linux）需使用**同一份 `vbus_api.h` 语义**解析 ring 与 blk。

---

## 4. 核心 API（`vbus.h`）

| API | 作用 |
|-----|------|
| **`rt_vbus_init(outr, inr)`** | 绑定**出/入 ring** 指针（通常指向共享内存中的 **`struct rt_vbus_ring`**） |
| **`rt_vbus_request_chn`** | 按 **`rt_vbus_request`**（名、优先级、server/client、水印）申请通道号 |
| **`rt_vbus_post`** | **异步投递**数据到某通道（可能仅入队，真正写入 ring 后通过 **`RT_VBUS_EVENT_ID_TX`** 通知） |
| **`rt_vbus_register_listener` / `rt_vbus_listen_on`** | 通道 **RX/TX/DISCONN** 事件 |
| **`rt_vbus_data_push` / `rt_vbus_data_pop`** | 在 **`vbus_chnx`** 设备路径上拼装/拆包 **`rt_vbus_data`** 链表 |
| **`rt_vbus_hw_init` / `rt_vbus_hw_eoi`** | **硬件/虚拟中断**侧初始化与 **EOI**（声明在 **`vbus.h`**，实现在 **`vbus_hw.h` 对应 BSP 移植文件**） |

**`vbus.c`**：ring 空间计算、**`chn0` 状态机**、与 **`prio_queue`** 结合的调度、virq 处理里调用 **`rt_vbus_hw_eoi`** 等。

---

## 5. `vbus_chnx.c` — 字符设备适配

将 **VBus 通道**包装为 **`rt_device`**：**`open`** 时 **`rt_vbus_request_chn`**，注册 **RX/TX** listener；**`read`/`write`** 通过 **`rt_vbus_data_pop`** 与内部缓冲配合，行为类似**串口流设备**，便于 **Finsh** 或 **FINSH 设备层** 挂接。

---

## 6. 辅助模块

| 文件 | 作用 |
|------|------|
| **`prio_queue.c/h`** | 多优先级发送队列，保证高优先级包先进入 ring |
| **`watermark_queue.c/h`** | 与 **`RT_VBUS_USING_FLOW_CONTROL`** 配合，按水位阻塞/唤醒 |

---

## 7. BSP / 对端移植要点

1. **`vbus_hw.h`**：本仓库 **`components/vbus` 下未包含该头文件**，需由 **BSP 或 hypervisor 移植层** 提供 **`rt_vbus_hw_init`、`rt_vbus_hw_eoi`** 及 **内存屏障宏**（**`rt_vbus_smp_wmb`/`rt_vbus_smp_rmb`** 等，见 **`vbus.c`** 引用）。  
2. **共享内存**：`_RT_VBUS_RING_BASE` 指向区域需 **两侧映射** 且 **缓存一致性**（非一致时需 **nocache 映射或显式 flush/inv**）。  
3. **virq**：一侧 **`post`** 后触发对端 **`RT_VBUS_HOST_VIRQ`/`GUEST_VIRQ`**，与 **GIC/PLIC/VMM** 配置一致。  
4. **RFS/RSHELL**：除 **`rfs.c`/`rshell.c`** 外，还需 **对端守护进程/内核模块** 与 **本侧 DFS/Finsh** 协同（属完整方案文档范畴）。

---

## 8. 与 `components` 文档中的关系

在 **`doc/RT-Thread-5.2.0-components-模块详解.md`** 中 VBus 一节为概要；本文档补充 **ring 布局、通道 0 协议、缺失 `utilities` 源文件** 与 **`vbus_hw` 外置** 等实现级注意点。

---

## 9. 相关文档与代码位置

- 头文件：**`components/vbus/share_hdr/vbus_api.h`**（建议与对端**同源同步**）。  
- **`components/vbus/README`**：若上游后续补充，可与本说明对照。

---

*文档对应源码树：`rt-thread-5.2.0/components/vbus`（5.2.0）；若你本地分支含 **`utilities/rfs.c`** 等，以实际文件为准。*
