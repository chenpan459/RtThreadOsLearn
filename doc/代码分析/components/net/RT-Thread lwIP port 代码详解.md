# RT-Thread lwIP port 代码详解

本文针对 `rt-thread-5.2.0/components/net/lwip/port` 目录，说明 RT-Thread 如何把上游 lwIP (这里是 `lwip-2.1.2`) 接到 RTOS 和底层网卡驱动。

## 1. 目录作用总览

- `SConscript`
  - 把本目录 `*.c` 作为 `lwIP` 组件编译，依赖宏是 `RT_USING_LWIP`。
- `sys_arch.c`
  - lwIP 的 `sys_arch` 适配实现，负责线程/信号量/邮箱/临界区等 OS 抽象。
  - 同时负责 lwIP 系统初始化入口 `lwip_system_init()`。
- `ethernetif.c`
  - RT-Thread 侧网络设备适配层，连接 `struct netif` 和 `struct eth_device`。
  - 实现收发线程（`etx`/`erx`）、`eth_device_init()`、`eth_device_ready()` 等关键逻辑。
- `netif/ethernetif.h`
  - 声明 `struct eth_device` 及网卡驱动与 port 层对接 API。
- `lwipopts.h`
  - lwIP 功能配置汇总，主要从 `rtconfig.h` 宏派生。
- `arch/cc.h`、`arch/sys_arch.h`
  - lwIP 需要的平台基础类型、打包宏、`sys_sem_t/sys_mbox_t` 等类型映射。
- `lwippools.h`
  - 自定义内存池定义（可选）。

## 2. 初始化路径（系统启动时发生什么）

### 2.1 lwIP 全局初始化

`sys_arch.c` 中 `lwip_system_init()` 通过 `INIT_PREV_EXPORT` 在组件初始化阶段执行：

1. 调 `eth_system_device_init_private()` 先创建以太网收发线程和邮箱。
2. 设置 `netif_default = NULL`。
3. 调用 `tcpip_init(...)` 启动 lwIP 的 `tcpip_thread`，并用信号量等待初始化完成。

这一步完成后，lwIP 核心线程和 RT-Thread port 层的收发线程都就绪。

### 2.2 网卡设备注册与 netif 绑定

板级驱动（例如 BSP 里的 SMC911x 驱动）会调用 `eth_device_init()`：

1. 分配 `struct netif`，绑定到 `eth_device->netif`。
2. 通过 `rt_device_register()` 把网卡注册到 RT-Thread 设备框架。
3. 设置 `netif->linkoutput = ethernetif_linkoutput`（非常关键）。
4. 若 `tcpip` 线程已经存在，则 `netifapi_netif_add(..., eth_netif_device_init, tcpip_input)`：
   - `eth_netif_device_init` 中执行 `rt_device_init/open`，设置 MTU、flags、`netif->output = etharp_output`，并处理 DHCP / link up。

## 3. 发包调用链（lwIP -> 驱动）

应用发包后，典型路径如下：

1. lwIP 协议栈进入 `ethernet_output()`（上游 `lwip-2.1.2/src/netif/ethernet.c`）。
2. `ethernet_output()` 填好二层头后调用 `netif->linkoutput(netif, p)`。
3. 在 RT-Thread port 中，这个回调是 `ethernetif_linkoutput()`：
   - 默认模式下，它把发送请求投递给 `eth_tx_thread_mb`，等待 `ack`。
4. `eth_tx_thread_entry()` 从邮箱取消息后，拿到 `netif->state` 对应的 `struct eth_device`，调用驱动的 `eth_tx()`。
5. 驱动将 `pbuf` 数据写入网卡硬件（或虚拟网卡）发送。

结论：**lwIP 并不直接操作硬件寄存器，而是通过 `linkoutput -> eth_tx` 下沉到 BSP 驱动。**

## 4. 收包调用链（驱动 -> lwIP）

典型路径如下：

1. 网卡中断触发后，驱动调用 `eth_device_ready(dev)`。
2. `eth_device_ready()` 向 `eth_rx_thread_mb` 投递设备消息（避免在中断里跑重逻辑）。
3. `eth_rx_thread_entry()` 被唤醒后循环调用 `device->eth_rx()` 拉取 `pbuf`。
4. 每拿到一个 `pbuf`，调用 `device->netif->input(p, device->netif)`，这里就是 `tcpip_input`。
5. 包进入 lwIP `tcpip_thread` 继续做 ARP/IP/TCP/UDP 处理。

结论：**中断只做通知，真正收包解析在 RX 线程 + tcpip 线程完成。**

## 5. `ethernetif.c` 关键模块详解

### 5.1 线程与邮箱模型

- TX 线程：`eth_tx_thread_entry`
  - 串行处理发送请求，避免并发直接触达驱动。
- RX 线程：`eth_rx_thread_entry`
  - 处理 link change 与 pbuf 上送。
- 邮箱：
  - `eth_tx_thread_mb`：发送请求队列。
  - `eth_rx_thread_mb`：接收通知与链路变化通知队列。

这种模型降低了中断上下文复杂度，同时把驱动交互集中在可调度线程上下文。

### 5.2 设备抽象 `struct eth_device`

在 `netif/ethernetif.h` 中定义，核心成员：

- `eth_rx`：驱动提供，返回接收到的 `pbuf`。
- `eth_tx`：驱动提供，发送 `pbuf`。
- `netif`：与 lwIP 绑定的网络接口对象。
- `link_status/link_changed/rx_notice`：链路状态与通知去重控制。

### 5.3 链路状态同步

- 驱动通过 `eth_device_linkchange(dev, up/down)` 上报链路变化。
- RX 线程处理该状态并调用 `netifapi_netif_set_link_up/down()`。
- 若启用了 `RT_USING_NETDEV`，还会同步到 `netdev` 层（`netdev_flags_sync`）。

## 6. `sys_arch.c` 关键点（OS 抽象层）

`sys_arch.c` 实现了 lwIP 所需系统接口：

- 信号量：`sys_sem_new/free/signal/sys_arch_sem_wait`
- 邮箱：`sys_mbox_new/free/post/fetch/...`
- 互斥：`sys_mutex_new/lock/unlock/free`
- 线程：`sys_thread_new`
- 临界区保护：`sys_arch_protect/unprotect`

这些接口都映射到 RT-Thread 的 IPC 与调度原语，是 lwIP 在 RTOS 上正常运行的根基。

## 7. `lwipopts.h` 如何影响行为

`lwipopts.h` 是本 port 的功能开关中心，常见影响项：

- 协议开关：`LWIP_TCP/LWIP_UDP/LWIP_DHCP/LWIP_DNS/LWIP_IPV6`
- 线程参数：`TCPIP_THREAD_PRIO/STACKSIZE/MBOX_SIZE`
- 内存池和缓存：`MEMP_NUM_*`、`PBUF_POOL_SIZE`、`MEM_ALIGNMENT`
- socket 兼容：`LWIP_COMPAT_SOCKETS`、`LWIP_SO_RCVTIMEO` 等

这些宏很多来自 `rtconfig.h`，因此要确保配置文件一致，避免“看起来关了但实际仍编译”的情况。

## 8. 驱动接入最小要求（给 BSP 驱动作者）

至少需要：

1. 准备 `struct eth_device` 实例。
2. 实现并赋值：
   - `dev.eth_tx = your_tx_func`
   - `dev.eth_rx = your_rx_func`
3. 支持 `NIOCTL_GADDR` 返回 MAC 地址。
4. 调用 `eth_device_init(&dev, "e0")` 注册。
5. 收到包时调用 `eth_device_ready(&dev)` 通知 RX 线程。
6. 链路变化时调用 `eth_device_linkchange(&dev, RT_TRUE/RT_FALSE)`。

## 9. 常见问题与排查

- 现象：`list_if` 看不到网卡
  - 检查是否调用了 `eth_device_init()`，以及 `RT_USING_LWIP` 是否开启。
- 现象：能初始化但不收包
  - 检查中断里是否调用 `eth_device_ready()`，`eth_rx()` 是否正确返回 `pbuf`。
- 现象：能收包但不发包
  - 检查 `eth_tx()` 是否正确处理 `pbuf` 链与长度，是否错误释放内存。
- 现象：`.config` 与运行行为不一致
  - 检查 `rtconfig.h` 是否和 `.config` 同步重生成并全量重编译。

## 10. 一句话总结

`components/net/lwip/port` 的核心价值是：  
**把上游 lwIP 的 `netif/sys_arch` 两类抽象，稳定映射到 RT-Thread 的线程/IPC/设备模型，再通过 `eth_device` 回调接到 BSP 网卡驱动。**

