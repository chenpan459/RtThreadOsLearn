# RT-Thread 5.2.0 WLAN（Wi-Fi 设备与协议栈框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/wlan` 目录实现的 **Wi-Fi 框架**：在 **`RT_USING_WIFI`** 下参与编译，通过 **`struct rt_wlan_device` + `struct rt_wlan_dev_ops`** 抽象 **芯片侧驱动**；在其上叠加 **连接管理（`dev_wlan_mgnt.c`）**、**传输协议插件（`dev_wlan_prot.c`，默认 LwIP）**、**配置持久化（`dev_wlan_cfg.c`）**、**专用工作队列（`dev_wlan_workqueue.c`）** 与 **MSH 命令（`dev_wlan_cmd.c`）**。

**`rtdevice.h`** 在开启 Wi-Fi 时包含聚合头 **`drivers/wlan.h`**（其再包含各 **`dev_wlan*.h`**）：

```232:234:rt-thread-5.2.0/components/drivers/include/rtdevice.h
#ifdef RT_USING_WIFI
#include "drivers/wlan.h"
#endif /* RT_USING_WIFI */
```

**`components/drivers/include/drivers/wlan.h`** 与 **`components/drivers/wlan/*.h`** 并列：`SConscript` 将 **`components/drivers/wlan`** 加入 **`CPPPATH`**，因此应用代码可写 **`#include <dev_wlan.h>`** 或 **`#include <drivers/wlan.h>`**（后者经 **`rtdevice.h`** 拉取）。

---

## 1. 目录与编译（`SConscript`）

| 源文件 | 条件 | 作用 |
|--------|------|------|
| **`dev_wlan.c`** | 始终（依赖 **`RT_USING_WIFI`**） | **`rt_wlan_dev_*`、`rt_device` 封装、`RT_WLAN_CMD_*` 分发、`rt_wlan_dev_register`、驱动事件回调表** |
| **`dev_wlan_mgnt.c`** | **`RT_WLAN_MANAGE_ENABLE`** | **`rt_wlan_*` 高层 API、STA/AP 描述符、完成量等待连接/扫描、`rt_wlan_prot_ready_event`** |
| **`dev_wlan_cmd.c`** | **`RT_WLAN_MSH_CMD_ENABLE`**（位于 **`RT_WLAN_MANAGE_ENABLE`** 子菜单内） | **`wifi` MSH 子命令（scan/join/ap 等）** |
| **`dev_wlan_prot.c`** | **`RT_WLAN_PROT_ENABLE`** | **协议全局表 `_prot[]`、attach/detach、与 `RT_WLAN_DEV_EVT_*` 的桥接、`prot_recv` / `wlan_send` 转发** |
| **`dev_wlan_lwip.c`** | **`RT_WLAN_PROT_LWIP_ENABLE`**（**`select RT_USING_LWIP`**） | **注册名为 `RT_WLAN_PROT_LWIP_NAME`（默认 `lwip`）的协议：`eth_device` + netif + 可选 `netdev`** |
| **`dev_wlan_cfg.c`** | **`RT_WLAN_CFG_ENABLE`** | **多条 `rt_wlan_cfg_info` 内存缓存 + 带头与 CRC 的持久化序列化；`rt_wlan_cfg_set_ops` 注入读写后端** |
| **`dev_wlan_workqueue.c`** | **`RT_WLAN_WORK_THREAD_ENABLE`** | **`rt_workqueue` 单例、`rt_wlan_workqueue_dowork`；`INIT_PREV_EXPORT` 早于业务初始化** |

**`SConscript`** 将上述文件按 **`GetDepend([...])`** 追加到 **`DeviceDrivers`** 组，**`depend = ['RT_USING_WIFI']`**，**`CPPPATH = [cwd]`**（即 **`wlan` 目录**）。

---

## 2. Kconfig 要点（`wlan/Kconfig`）

| 选项 | 含义 |
|------|------|
| **`RT_USING_WIFI`** | 总开关；本目录参与编译 |
| **`RT_WLAN_DEVICE_STA_NAME` / `RT_WLAN_DEVICE_AP_NAME`** | 默认 **`wlan0` / `wlan1`**，与 **`rt_device_find`**、**`rt_wlan_set_mode`** 使用的逻辑名一致 |
| **`RT_WLAN_SSID_MAX_LENGTH` / `RT_WLAN_PASSWORD_MAX_LENGTH`** | 与 **`dev_wlan.h`** 中 **`#ifndef` 默认** 对齐 |
| **`RT_WLAN_DEV_EVENT_NUM`** | 每个 **`RT_WLAN_DEV_EVT_*` 槽位可挂的回调个数**（默认 2） |
| **`RT_WLAN_MANAGE_ENABLE`** | 编译 **`dev_wlan_mgnt.c`**；子项含扫描/连接超时、**`RT_WLAN_SCAN_SORT`**、**`RT_WLAN_JOIN_SCAN_BY_MGNT`**、**`RT_WLAN_MSH_CMD_ENABLE`** |
| **`RT_WLAN_AUTO_CONNECT_ENABLE`** | **`select RT_WLAN_CFG_ENABLE`** 与 **`RT_WLAN_WORK_THREAD_ENABLE`**；周期 **`AUTO_CONNECTION_PERIOD_MS`** |
| **`RT_WLAN_CFG_ENABLE` / `RT_WLAN_CFG_INFO_MAX`** | 持久化条目上限（默认 3） |
| **`RT_WLAN_PROT_ENABLE`** | **`RT_WLAN_PROT_MAX`**（默认 2）、**`RT_WLAN_PROT_NAME_LEN`**、**`RT_WLAN_DEFAULT_PROT`** |
| **`RT_WLAN_PROT_LWIP_ENABLE`** | LwIP 协议；**`RT_WLAN_PROT_LWIP_PBUF_FORCE`** 时 **`rt_wlan_prot_attach_dev`** 仅允许名为 **`RT_WLAN_PROT_LWIP_NAME`** 的协议 |
| **`RT_WLAN_WORK_THREAD_ENABLE`** | 工作队列线程名/栈/优先级 |
| **`RT_WLAN_DEBUG`** | 各子文件 **`DBG_LVL`** 细分 |

---

## 3. 命令字与驱动事件（`dev_wlan.h`）

**控制命令**从 **`0x10`** 起递增，经 **`rt_device_control(wlan_dev, cmd, args)`** 进入 **`_rt_wlan_dev_control`**：

```22:72:rt-thread-5.2.0/components/drivers/wlan/dev_wlan.h
typedef enum
{
    RT_WLAN_CMD_MODE = 0x10,
    RT_WLAN_CMD_SCAN,
    RT_WLAN_CMD_JOIN,
    RT_WLAN_CMD_SOFTAP,
    RT_WLAN_CMD_DISCONNECT,
    RT_WLAN_CMD_AP_STOP,
    RT_WLAN_CMD_AP_DEAUTH,
    RT_WLAN_CMD_SCAN_STOP,
    RT_WLAN_CMD_GET_RSSI,
    RT_WLAN_CMD_GET_INFO,
    RT_WLAN_CMD_AP_GET_INFO,
    RT_WLAN_CMD_SET_POWERSAVE,
    RT_WLAN_CMD_GET_POWERSAVE,
    RT_WLAN_CMD_CFG_PROMISC,
    RT_WLAN_CMD_CFG_FILTER,
    RT_WLAN_CMD_CFG_MGNT_FILTER,
    RT_WLAN_CMD_SET_CHANNEL,
    RT_WLAN_CMD_GET_CHANNEL,
    RT_WLAN_CMD_SET_COUNTRY,
    RT_WLAN_CMD_GET_COUNTRY,
    RT_WLAN_CMD_SET_MAC,
    RT_WLAN_CMD_GET_MAC,
    RT_WLAN_CMD_GET_FAST_CONNECT_INFO,
    RT_WLAN_CMD_FAST_CONNECT,
} rt_wlan_cmd_t;

typedef enum
{
    RT_WLAN_DEV_EVT_INIT_DONE = 0,
    RT_WLAN_DEV_EVT_CONNECT,
    RT_WLAN_DEV_EVT_CONNECT_FAIL,
    RT_WLAN_DEV_EVT_DISCONNECT,
    RT_WLAN_DEV_EVT_AP_START,
    RT_WLAN_DEV_EVT_AP_STOP,
    RT_WLAN_DEV_EVT_AP_ASSOCIATED,
    RT_WLAN_DEV_EVT_AP_DISASSOCIATED,
    RT_WLAN_DEV_EVT_AP_ASSOCIATE_FAILED,
    RT_WLAN_DEV_EVT_SCAN_REPORT,
    RT_WLAN_DEV_EVT_SCAN_DONE,
    RT_WLAN_DEV_EVT_MAX,
} rt_wlan_dev_event_t;
```

**安全能力**以 **`WPA_SECURITY` / `WPA2_SECURITY` / `WEP_ENABLED` 等位掩码** 组合成 **`rt_wlan_security_t`**；**`rt_country_code_t`** 为国家/地区枚举（体量较大，见头文件）。

---

## 4. `dev_wlan.c`：设备层

### 4.1 初始化与互斥

- **`_rt_wlan_dev_init`**：初始化 **`wlan->lock`**，调用 **`ops->wlan_init`**。
- **`_rt_wlan_dev_control`**：在 **`WLAN_DEV_LOCK`** 下 **`switch (cmd)`**，将各 **`RT_WLAN_CMD_*`** 转为 **`wlan->ops->wlan_*`**；未实现的 **`ops`** 成员可能导致 **`-RT_ERROR`** 或 **不修改输出参数**（视分支而定）。

### 4.2 `rt_wlan_dev_register`

- 设备类型登记为 **`RT_Device_Class_NetIf`**（与 **`netdev` / LwIP eth** 语义一致）。
- **`RT_USING_DEVICE_OPS`** 时使用静态 **`wlan_ops`**，否则直接填 **`device.init` / `device.control`**。
- **`flag`** 可同时携带 **`RT_WLAN_FLAG_STA_ONLY` / `RT_WLAN_FLAG_AP_ONLY`** 以约束 **`rt_wlan_set_mode`** 中的模式切换；二者互斥时 **注册失败**。

```995:1031:rt-thread-5.2.0/components/drivers/wlan/dev_wlan.c
rt_err_t rt_wlan_dev_register(struct rt_wlan_device *wlan, const char *name, const struct rt_wlan_dev_ops *ops, rt_uint32_t flag, void *user_data)
{
    rt_err_t err = RT_EOK;

    if ((wlan == RT_NULL) || (name == RT_NULL) || (ops == RT_NULL) ||
        (flag & RT_WLAN_FLAG_STA_ONLY && flag & RT_WLAN_FLAG_AP_ONLY))
    {
        LOG_E("F:%s L:%d parameter Wrongful", __FUNCTION__, __LINE__);
        return RT_NULL;
    }

    rt_memset(wlan, 0, sizeof(struct rt_wlan_device));

#ifdef RT_USING_DEVICE_OPS
    wlan->device.ops = &wlan_ops;
#else
    wlan->device.init       = _rt_wlan_dev_init;
    wlan->device.open       = RT_NULL;
    wlan->device.close      = RT_NULL;
    wlan->device.read       = RT_NULL;
    wlan->device.write      = RT_NULL;
    wlan->device.control    = _rt_wlan_dev_control;
#endif

    wlan->device.user_data  = RT_NULL;

    wlan->device.type = RT_Device_Class_NetIf;

    wlan->ops = ops;
    wlan->user_data  = user_data;

    wlan->flags = flag;
    err = rt_device_register(&wlan->device, name, RT_DEVICE_FLAG_RDWR);

    LOG_D("F:%s L:%d run", __FUNCTION__, __LINE__);

    return err;
}
```

说明：参数非法分支 **`return RT_NULL`**，与函数返回类型 **`rt_err_t`** 不一致，调用方若按 **`rt_err_t`** 判断可能被误判；属源码层面的 API 瑕疵。

### 4.3 事件 API

- **`rt_wlan_dev_register_event_handler(wlan, event, handler, parameter)`**：在 **`handler_table[event][0..RT_WLAN_DEV_EVENT_NUM-1]`** 中寻找空槽登记。
- **`rt_wlan_dev_indicate_event_handle`**：驱动在固件状态变化时调用，**遍历该事件所有已登记 handler**。

---

## 5. `dev_wlan_mgnt.c`：连接管理

### 5.1 初始化与 STA/AP 绑定

- **`rt_wlan_init`**：**`INIT_PREV_EXPORT`**，仅初始化 **互斥量、完成量表、STA 链表头、自动重连定时器（若开启）** 等；**不**自动 **`rt_device_find`**。
- **`rt_wlan_set_mode(dev_name, mode)`**：**`rt_device_find`** 后对 **`rt_wlan_dev_init(device, mode)`**；当 **`mode == RT_WLAN_STATION`** 时将 **`_sta_mgnt.device`** 指向该 **`rt_wlan_device`**，并为 STA 路径注册 **`rt_wlan_event_dispatch`** 到 **`RT_WLAN_DEV_EVT_CONNECT` / `CONNECT_FAIL` / `DISCONNECT` / `SCAN_*`**；AP 模式同理绑定 **`_ap_mgnt.device`** 与 AP 相关事件。
- **`rt_wlan_connect`** 等 API 在 **`_sta_mgnt.device == RT_NULL`** 时直接 **`-RT_EIO`**，因此应用或板级初始化需 **先 `rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION)`（或等价逻辑名）**。

### 5.2 `rt_wlan_event_dispatch`

将 **`RT_WLAN_DEV_EVT_*`** 映射为面向应用的 **`RT_WLAN_EVT_*`**，并：

- 更新 **`_sta_mgnt.state` / `_ap_mgnt.state`**（**`RT_WLAN_STATE_CONNECT`、`RT_WLAN_STATE_READY`** 等）；
- **`COMPLETE_LOCK`** 下唤醒所有 **`complete_tab`** 中的 **`rt_event`**（供 **`rt_wlan_complete_wait`** 使用的 **连接/扫描同步**）；
- 若定义 **`RT_WLAN_WORK_THREAD_ENABLE`**：通过 **`rt_wlan_workqueue_dowork(rt_wlan_mgnt_work, msg)`** 在 **WLAN 工作线程** 中执行 **用户 `rt_wlan_register_event_handler` 回调** 及 **STA 连接后的配置保存**等副作用；否则在 **当前上下文** 直接调 **`event_tab`** 中的 handler。

### 5.3 `rt_wlan_prot_ready_event`（“协议就绪”与 `RT_WLAN_EVT_READY`）

由 **`dev_wlan_prot.c`** 中的 **`rt_wlan_prot_ready`** 转调 **`rt_wlan_mgnt.c`** 内实现，用于 **STA 已关联且 LwIP 已获得地址等“可发业务数据”** 的节点：

```1728:1761:rt-thread-5.2.0/components/drivers/wlan/dev_wlan_mgnt.c
int rt_wlan_prot_ready_event(struct rt_wlan_device *wlan, struct rt_wlan_buff *buff)
{
    rt_base_t level;

    if ((wlan == RT_NULL) || (_sta_mgnt.device != wlan) ||
            (!(_sta_mgnt.state & RT_WLAN_STATE_CONNECT)))
    {
        return -1;
    }
    if (_sta_mgnt.state & RT_WLAN_STATE_READY)
    {
        return 0;
    }
    level = rt_hw_interrupt_disable();
    _sta_mgnt.state |= RT_WLAN_STATE_READY;
    rt_hw_interrupt_enable(level);
#ifdef RT_WLAN_WORK_THREAD_ENABLE
    rt_wlan_send_to_thread(RT_WLAN_EVT_READY, buff->data, buff->len);
#else
    {
        void *user_parameter;
        rt_wlan_event_handler handler = RT_NULL;

        level = rt_hw_interrupt_disable();
        handler = event_tab[RT_WLAN_EVT_READY].handler;
        user_parameter = event_tab[RT_WLAN_EVT_READY].parameter;
        rt_hw_interrupt_enable(level);
        if (handler)
        {
            handler(RT_WLAN_EVT_READY, buff, user_parameter);
        }
    }
#endif
    return 0;
}
```

即：**仅当前 STA 管理设备**、且 **已处于 CONNECT 状态** 时，置 **`RT_WLAN_STATE_READY`** 并投递 **`RT_WLAN_EVT_READY`**。

### 5.4 其它管理行为（摘要）

- **`rt_wlan_connect`**：在 **`RT_WLAN_JOIN_SCAN_BY_MGNT`** 下先 **`rt_wlan_scan_with_info`** 填充 **`rt_wlan_info`**（含 BSSID/信道等），再 **`rt_wlan_connect_adv`** + **`rt_wlan_complete_wait`** 等待 **`CONNECT`/`CONNECT_FAIL`**。
- **`RT_WLAN_AUTO_CONNECT_ENABLE`**：**软定时器周期** 触发 **`rt_wlan_cyclic_check`**，结合 **`rt_wlan_cfg_*`** 尝试重连。

---

## 6. `dev_wlan_prot.c`：协议插件

- **全局表 **`_prot[RT_WLAN_PROT_MAX]`**：**`rt_wlan_prot_regisetr`** 登记 **`struct rt_wlan_prot`**（**`name` + `ops` + 分配 id**）；**`id`** 高 16 位为 **`RT_LWAN_ID_PREFIX`（`0x5054`）**，低 16 位为自增序号（头文件宏名为 **`RT_LWAN_ID_PREFIX`**，属历史命名）。
- **`rt_wlan_prot_attach_dev`**：按名查找已注册协议，调用 **`ops->dev_reg_callback`**，将返回的 **`struct rt_wlan_prot *`** 赋给 **`wlan->prot`**；并向 **`RT_WLAN_DEV_EVT_CONNECT` 等** 注册 **`rt_wlan_prot_event_handle`**，以便把 **驱动事件** 转为 **`RT_WLAN_PROT_EVT_*`** 并调用 **`rt_wlan_prot_event_register`** 登记的协议层 handler。
- **`rt_wlan_prot_transfer_dev`**：**`wlan->ops->wlan_send`**；**`rt_wlan_dev_transfer_prot`**：**`wlan->prot->ops->prot_recv`**。

头文件与实现中 **`rt_wlan_prot_regisetr`** 均为 **`regisetr` 拼写**，第三方协议模块需 **与此符号一致** 方可链接。

---

## 7. `dev_wlan_lwip.c`：默认 LwIP 协议

- **`INIT_PREV_EXPORT(rt_wlan_lwip_init)`**：填充静态 **`struct rt_wlan_prot prot`**（名 **`RT_WLAN_PROT_LWIP_NAME`**），**`rt_wlan_prot_regisetr`**，并对每个 **`RT_WLAN_PROT_EVT_*`** 调用 **`rt_wlan_prot_event_register(..., rt_wlan_lwip_event_handle)`**。
- **`dev_reg_callback`**：**`eth_device_init`**、绑定 **`eth_tx = rt_wlan_lwip_protocol_send`**，**`#ifdef RT_USING_NETDEV`** 时 **`wlan->netdev = netdev_get_by_name(eth_name)`**；可配合 **DHCP 服务端**（**`LWIP_USING_DHCPD`**）。
- **`netif_is_ready` 等工作项**：在 **IP 就绪** 等条件下调用 **`rt_wlan_prot_ready`**，进而触发上一节的 **`RT_WLAN_EVT_READY`** 链。

---

## 8. `dev_wlan_cfg.c`：配置缓存与持久化

- **`struct cfg_save_info_head`**：**`magic`（`RT_WLAN_CFG_MAGIC`）、`len`、`num`、`crc`**；**`crc`** 对 **`rt_wlan_cfg_info` 数组负载** 做 **CRC16-CCITT**。
- **`rt_wlan_cfg_cache_refresh`**：从 **`cfg_ops->read_cfg`** 读回、校验后合并进 **`cfg_cache`**（去重逻辑见源码循环）。
- **`rt_wlan_cfg_cache_save`**：打包头部 + 当前缓存，**`cfg_ops->write_cfg`**。
- 若未 **`rt_wlan_cfg_set_ops`**，部分 API 会 **提前返回** 或 **不写介质**（见各函数开头判断）。

---

## 9. `dev_wlan_workqueue.c`

- **`INIT_PREV_EXPORT(rt_wlan_workqueue_init)`**：**`rt_workqueue_create`**，线程名/栈/优先级来自 Kconfig。
- **`rt_wlan_workqueue_dowork`**：动态分配 **`struct rt_wlan_work`**，**`rt_work_init`** 后 **`rt_workqueue_dowork`**；工作函数执行完 **`rt_free(wlan_work)`**。

---

## 10. `dev_wlan_cmd.c`

- 编译条件：**`#if defined(RT_WLAN_MANAGE_ENABLE) && defined(RT_WLAN_MSH_CMD_ENABLE)`**。
- 子命令表 **`cmd_tab`**：**`scan`、`join`、`ap`、`disc`、`ap_stop`、`status`、`list_sta`** 等；**`MSH_CMD_EXPORT_ALIAS(wifi_msh, wifi, wifi command)`** 导出顶层 **`wifi`**。
- **`RT_WLAN_CMD_DEBUG`** 下增加 **`wifi -d ...`** 调试子命令（保存/清空配置、dump 协议等）。

---

## 11. 驱动移植与集成顺序（建议）

1. 实现 **`struct rt_wlan_dev_ops`**，填充 **`rt_wlan_device`** 后 **`rt_wlan_dev_register(..., RT_WLAN_DEVICE_STA_NAME, ...)`**（或与 Kconfig 一致的名称）。
2. 若使用管理框架：**`rt_wlan_set_mode("wlan0", RT_WLAN_STATION)`**（及可选 AP 设备 **`rt_wlan_set_mode("wlan1", RT_WLAN_AP)`**）。
3. 若使用 LwIP：**`rt_wlan_prot_attach("wlan0", "lwip")`**（或 **`RT_WLAN_PROT_LWIP_NAME`** 实际字符串）；确保 **`rt_wlan_lwip_init`** 已通过 **`INIT_PREV_EXPORT`** 注册 **`lwip` 协议**。
4. 收包路径：驱动将 **802.3 帧** 递交 **`rt_wlan_dev_transfer_prot`** 或等价 **`prot_recv`** 入口；发包经 **`rt_wlan_prot_transfer_dev`** 或 **`ops->wlan_send`**。
5. 状态上报：在关联、断线、扫描结束等时机调用 **`rt_wlan_dev_indicate_event_handle`**；LwIP 侧在 **IP 就绪** 时调用 **`rt_wlan_prot_ready`**。
6. 持久化：在板级 **`rt_wlan_cfg_set_ops`** 指向 **Flash 读写** 实现。

---

## 12. 小结

| 层次 | 主要文件 | 职责 |
|------|-----------|------|
| **设备抽象** | **`dev_wlan.h` / `dev_wlan.c`** | **`RT_WLAN_CMD_*`、互斥、`rt_wlan_dev_ops`、驱动级事件表、注册为 `NetIf` 类设备** |
| **连接管理** | **`dev_wlan_mgnt.*`** | **`rt_wlan_set_mode` 绑定 STA/AP、`rt_wlan_event_dispatch`、完成量、自动重连、`rt_wlan_prot_ready_event`** |
| **协议插件** | **`dev_wlan_prot.*`、`dev_wlan_lwip.c`** | **多协议表、attach、数据面转发、LwIP/eth/netdev 默认实现** |
| **配置** | **`dev_wlan_cfg.*`** | **内存缓存 + 带头 CRC 的序列化、`cfg_ops` 注入** |
| **异步执行** | **`dev_wlan_workqueue.*`** | **管理模块与用户回调在独立线程执行** |
| **Shell** | **`dev_wlan_cmd.c`** | **`wifi` MSH** |

**射频与固件接口**仍由各 **Wi-Fi 驱动/BSP** 在 **`rt_wlan_dev_ops`** 中实现；本目录提供 **统一的设备模型、命令字、事件与 LwIP 绑定范式**。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/wlan/`。*
