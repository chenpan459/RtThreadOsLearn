# RT-Thread 5.2.0 `components/net` 模块详细分析

**`components/net`** 提供 RT-Thread 的**网络栈集成**：**网卡抽象（netdev）**、**Socket 抽象层（SAL）**、**lwIP 多版本移植**、**AT 命令框架**、以及 **DHCP 服务端**、**IPv4 NAT** 等扩展。典型数据路径为：

**应用** →（可选 **`libc` POSIX`）** **`socket`/`select`** → **SAL** → **lwIP（或 AT 虚拟栈）** → **`netdev` 注册的以太网/WiFi 驱动** → **BSP 网卡设备**。

---

## 1. 目录与构建入口

**`components/net/SConscript`**：对子目录 **`sal`、`netdev`、`lwip`、`at`、`lwip-dhcpd`、`lwip-nat`** 等凡存在 **`SConscript`** 的一级目录递归合并。

**`components/net/Kconfig`**：`rsource` **sal、netdev、lwip、at**（**DHCPD/NAT** 的开关在 **`lwip/Kconfig`** 或 **`rtconfig.h`** 中配合，见下文）。

---

## 2. `netdev/` — 网络接口设备抽象

**作用**：统一描述一块网卡：**名称、IP/掩码/网关、链路 up/down、回调** 等，供 **lwIP `netif`**、**SAL**、**ping/ifconfig/netstat` MSH 命令** 使用。

**`netdev/Kconfig`**：**`RT_USING_NETDEV`**；子选项如 **`NETDEV_USING_IFCONFIG`/`PING`/`NETSTAT`**、**`NETDEV_USING_AUTO_DEFAULT`**（默认网卡自动切换）、**`NETDEV_USING_IPV6`** 等。

**`netdev/SConscript`**：编译 **`src/*.c`**，**`CPPPATH`** 含 **`include`**。  
**注意**：脚本里 **`DefineGroup` 的组名写为 `'SAL'`**（与上游一致，易混淆），实际仍是 **netdev** 对象实现；**`depend = ['RT_USING_NETDEV']`**。

**与 SAL**：**`RT_USING_SAL`** 在 **`sal/Kconfig`** 中 **`select RT_USING_NETDEV`**，即开 SAL 会带上 netdev。

---

## 3. `sal/` — Socket Abstraction Layer

**`RT_USING_SAL`**：在 BSD **`socket`/`connect`/`send`** 等 API 之下，把调用转发到 **具体协议栈适配层**。

**`sal/SConscript`** 要点：

| 条件 | 增加源码/路径 |
|------|----------------|
| 始终 | **`src/*.c`**、**`socket/net_netdb.c`**；**`include`**、**`include/socket`** |
| **`SAL_USING_LWIP` 或 `SAL_USING_AT`** | **`CPPPATH += impl`** |
| **`SAL_USING_LWIP`** | **`impl/af_inet_lwip.c`** |
| **`SAL_USING_AT`** | **`impl/af_inet_at.c`** |
| **`SAL_USING_TLS`** | **`impl/proto_mbedtls.c`** |
| **`SAL_USING_POSIX`** | **`include/dfs_net`**、**`socket/net_sockets.c`**、**`dfs_net/*.c`** —— 与 **`DFS_USING_POSIX`** 配合，使 **socket 作为 fd** 参与 **`read`/`write`/`select`/`poll`**（**`sal/Kconfig`** 中 **`SAL_USING_POSIX` depends on DFS_USING_POSIX`，默认 y） |
| **`HAVE_SYS_SOCKET_H` 未定义** | 额外 **`include/socket/sys_socket`** 提供系统头兼容 |

**`sal/Kconfig`** 摘要：

- **`SAL_INTERNET_CHECK`**：联网探测，**`select RT_USING_SYSTEM_WORKQUEUE`**。  
- **`SAL_SOCKETS_NUM`**：在未走 POSIX fd 路径时的最大 socket 数（**`depends on !SAL_USING_POSIX`**）。

---

## 4. `lwip/` — lwIP 协议栈

### 4.1 版本选择

**`lwip/Kconfig`** 中 **`choice`**：**`RT_USING_LWIP141` / `RT_USING_LWIP203` / `RT_USING_LWIP212`**，以及可选 **`RT_USING_LWIP_LATEST`（PKG）**；**`RT_USING_LWIP_VER_NUM`** 为内部版本十六进制常量。

各版本目录 **`lwip-1.4.1`、`lwip-2.0.3`、`lwip-2.1.2`** 的 **`SConscript`** 使用 **`DefineGroup(..., depend=['RT_USING_LWIP', 'RT_USING_LWIPxxx'])`**，保证**只编译选中版本**的源码集合。

### 4.2 源码体量

以 **`lwip-2.1.2/SConscript`** 为例：拆分 **`lwipcore_SRCS`**（TCP/UDP/IP/pbuf 等）、**`lwipcore4_SRCS`**（IPv4 DHCP/ARP/ICMP…）、可选 **IPv6、SNMP、PPP、ping** 等；**`CPPPATH`** 指向对应 **`src/include`**；若 **未** 开 **`RT_USING_SAL`**，会加入 **compat posix** 头路径以便裸用 lwIP API。

### 4.3 `lwip/port/`

**`port/*.c`**、**`lwipopts.h`**：与 **RT-Thread 线程、邮箱、互斥** 及 **MEM/PBUF 池大小** 的移植；**`LWIP_USING_NAT`** 等宏在 **`lwipopts.h`** 中与 **`lwip-nat`** 组件联动。

### 4.4 Kconfig 常见项（节选）

- **静态 IPv4**：**`RT_LWIP_IPADDR`/`GWADDR`/`MSKADDR`**。  
- **协议**：**`RT_LWIP_TCP`/`UDP`/`ICMP`/`IGMP`/`DNS`/`DHCP`** 等。  
- **`RT_LWIP_USING_PING`**：**`select NETDEV_USING_PING`** 等。  
- **`LWIP_USING_DHCPD`**：启用 **DHCP 服务器**（驱动 **`lwip-dhcpd`** 子目录）。  
- **调试**：**`RT_LWIP_DEBUG`** 下大量子模块 log 开关。

---

## 5. `lwip-dhcpd/` — DHCP 服务器

**`SConscript`**：

- **`RT_USING_LWIP141`**：**`dhcp_server.c`**（旧 API）。  
- 否则：**`dhcp_server_raw.c`**（RAW API 路径）。

**依赖**：**`RT_USING_LWIP`** + **`LWIP_USING_DHCPD`**。  
**`Kconfig`**（在 **`lwip` 菜单内**）：可配置 **服务器 IP**、是否分配网关作路由器、自定义 DNS 等。

---

## 6. `lwip-nat/` — IPv4 NAT

**源码**：**`ipv4_nat.c`** 等，**`DefineGroup`** 依赖 **`RT_USING_LWIP`** + **`LWIP_USING_NAT`**。

**`lwip/Kconfig` 中未必有 NAT 菜单项**；**`lwip-nat/README.md`** 说明需在 **`rtconfig.h`（或等价配置）中定义 `LWIP_USING_NAT`**，并与 **`port/lwipopts.h`** 中相关宏一致。

---

## 7. `at/` — AT 命令组件

**`RT_USING_AT`**：模组 **AT Server/Client**、可选 **Finsh CLI**、**原始命令打印** 等。

**`at/SConscript`**：

- 基础：**`at_utils.c`**。  
- **`AT_USING_CLI`**：**`at_cli.c`**。  
- **`AT_USING_SERVER`**：**`at_server.c`、`at_base_cmd.c`**。  
- **`AT_USING_CLIENT`**：**`at_client.c`**。  
- **`AT_USING_SOCKET`**：**`at_socket/*.c`**，并在 **`at/Kconfig`** 中 **`select RT_USING_SAL`、`SAL_USING_AT`** —— 把 **蜂窝模组** 映射为 **BSD socket**。

**依赖 Finsh**：**`AT_USING_CLI`** 依赖 **`RT_USING_FINSH`**。

---

## 8. 配置依赖关系（简图）

```text
RT_USING_LWIP
  ├── select RT_USING_DEVICE, RT_USING_DEVICE_IPC
  ├── RT_USING_SAL → select SAL_USING_LWIP
  └── 版本三选一：LWIP141 / 203 / 212（或 PKG latest）

RT_USING_SAL
  ├── select RT_USING_NETDEV
  ├── SAL_USING_POSIX → depends DFS_USING_POSIX
  └── impl: LWIP / AT / TLS(MbedTLS)

RT_USING_AT
  └── AT_USING_SOCKET → SAL + SAL_USING_AT
```

---

## 9. 与 `drivers`、`libc` 的衔接

- **网卡驱动**：在 **`drivers`**（如以太网 MAC、**`wlan`**）或 **BSP** 中 **`rt_hw_xxx_register`**，并挂接 **`netdev`** 层 **`ethernetif`** 输入（具体函数以 lwIP port 为准）。  
- **POSIX**：**`RT_USING_POSIX_SOCKET`**（在 **`libc/posix/Kconfig`**）**`select SAL`**，使应用 **`#include <sys/socket.h>`** 与 **DFS fd** 一致。  
- **lwIP 内存**：**`MEM_ALIGNMENT`**、**`memp`** 数量等影响 RAM 占用，需与 **heap** 总量一起评估。

---

## 10. 阅读顺序建议

1. **`netdev/include`**：网卡对象与 API。  
2. **`sal/include`** + **`impl/af_inet_lwip.c`**：socket 如何调到 **`netconn`/`lwip`**。  
3. **`lwip/port/lwipopts.h`** 与 **`arch/sys_arch.c`**（在 port 目录）：OS 适配。  
4. **`lwip-2.1.2/src/api`**（若用 API 层）：`socket` 与 **`netconn`** 关系。  
5. **`at`**：从 **`at_client`** 到 **`at_socket`** 的数据路径。

---

## 11. 相关文档

- **`components/net/lwip/port/README.md`**  
- **`components/net/lwip-nat/README.md`**  
- **`doc/RT-Thread-5.2.0-libc-模块详细分析.md`**（POSIX socket）  
- **`doc/RT-Thread-5.2.0-drivers-模块详细分析.md`**（**`phy`/`wlan`**）

---

*文档对应源码树：`rt-thread-5.2.0/components/net`（5.2.0）。*
