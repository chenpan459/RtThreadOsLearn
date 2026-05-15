# RT-Thread 5.2.0 Watchdog（看门狗设备框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/watchdog` 目录实现的 **看门狗（WDT）设备框架**：在 **`RT_USING_WDT`** 下编译 **`dev_watchdog.c`**，对外头文件为 **`components/drivers/include/drivers/dev_watchdog.h`**；**`rtdevice.h`** 在 **`RT_USING_WDT`** 下包含 **`drivers/dev_watchdog.h`**。

本目录 **仅提供统一 `rt_device` 封装与标准 `control` 命令号**，**不包含具体 SoC 寄存器操作**；**硬件驱动** 在 **BSP / 板级驱动** 中实现 **`struct rt_watchdog_ops`** 并调用 **`rt_hw_watchdog_register`**。

---

## 1. 目录与编译

| 文件 | 作用 |
|------|------|
| **`dev_watchdog.c`** | **`rt_hw_watchdog_register` 与 `rt_device` 的 `init/open/close/control` 桥接** |
| **`Kconfig`** | **`RT_USING_WDT`**（默认 **n**） |
| **`SConscript`** | **`Glob('*.c')`**，**`CPPPATH`** 指向 **`../include`**，依赖 **`RT_USING_WDT`** |

---

## 2. 数据结构与 API（`dev_watchdog.h`）

### 2.1 `struct rt_watchdog_device`

- **`struct rt_device parent`**：注册为 **`RT_Device_Class_WDT`**。
- **`const struct rt_watchdog_ops *ops`**：底层实现指针。

### 2.2 `struct rt_watchdog_ops`

| 成员 | 是否可为 `NULL` | 含义 |
|------|-----------------|------|
| **`init`** | 可选 | **`rt_device_init` 时调用**；返回 **`-RT_ENOSYS`** 表示未实现 |
| **`control`** | **必选**（框架直接调用） | 处理 **`RT_DEVICE_CTRL_WDT_*`** 及驱动私有命令 |

### 2.3 标准 `control` 命令（**超时单位：秒**）

| 宏 | 含义 |
|----|------|
| **`RT_DEVICE_CTRL_WDT_GET_TIMEOUT`** | 查询超时时间（**秒**） |
| **`RT_DEVICE_CTRL_WDT_SET_TIMEOUT`** | 设置超时时间（**秒**），**`args` 一般为 `rt_uint32_t *`** |
| **`RT_DEVICE_CTRL_WDT_GET_TIMELEFT`** | 复位前剩余时间（**秒**） |
| **`RT_DEVICE_CTRL_WDT_KEEPALIVE`** | **喂狗 / 刷新计数** |
| **`RT_DEVICE_CTRL_WDT_START`** | **启动**看门狗 |
| **`RT_DEVICE_CTRL_WDT_STOP`** | **停止**看门狗（若硬件不支持则返回非 **`RT_EOK`**） |

### 2.4 `rt_hw_watchdog_register`

**`rt_device_register(device, name, flag)`**，无 **`RT_DEVICE_FLAG_STANDALONE`** 等与 **serial** 类似的强制附加标志（由 **`flag` 参数** 完全由调用方决定）。

---

## 3. `dev_watchdog.c` 行为说明

### 3.1 `rt_watchdog_init`

- 若 **`ops->init` 非空**：返回 **`ops->init(wtd)`**。
- 否则返回 **`-RT_ENOSYS`**。

即：**首次 `rt_device_init(dev)`** 时完成 **硬件相关的一次性初始化**（若驱动需要）。

### 3.2 `rt_watchdog_open`

- **恒返回 `RT_EOK`**，**不**自动 **START** 或 **SET_TIMEOUT**；是否启动由 **应用层 `rt_device_control`** 或 **驱动 probe 后逻辑** 决定。

### 3.3 `rt_watchdog_close`

- 调用 **`ops->control(wtd, RT_DEVICE_CTRL_WDT_STOP, RT_NULL)`**。
- 若 **失败**：打印 **`This watchdog can not be stoped`**（拼写为源码原样 **stoped**），返回 **`-RT_ERROR`**。
- 用于 **硬件不可关闭** 的看门狗（关闭设备节点时仍可能保持运行）。

### 3.4 `rt_watchdog_control`

- **原样转发** 到 **`wtd->ops->control(wtd, cmd, args)`**。

---

## 4. BSP 驱动编写约定

1. **实现 `control`**：至少支持 **`KEEPALIVE`**、**`START`/`STOP`**（若芯片支持）、**`SET_TIMEOUT`/`GET_TIMEOUT`**（若可配置）。
2. **`SET_TIMEOUT`/`GET_TIMELEFT`** 的 **`arg`** 建议约定为 **`rt_uint32_t *`** 指向 **秒**（与头文件注释一致）；若硬件以 **时钟 tick** 为单位，驱动内自行换算。
3. **多实例**：每个 **`rt_watchdog_device`** 独立 **`ops`** 与 **`user_data`**（**`register` 最后一个参数**）。
4. **与 idle 钩子配合**：框架 **不** 内置 **线程 idle 自动喂狗**；应用通常在 **`rt_thread_idle_sethook`** 或 **业务心跳** 中周期性 **`RT_DEVICE_CTRL_WDT_KEEPALIVE`**。

---

## 5. 典型使用顺序（应用侧）

1. **`rt_device_find("wdt")`**（名称由 BSP **`register` 字符串** 决定）。
2. **`rt_device_init`**（触发 **`ops->init`**，若有）。
3. **`rt_device_control(..., SET_TIMEOUT, &sec)`** → **`START`**。
4. 运行中周期性 **`KEEPALIVE`**；需要关机维护时 **`STOP`**（若支持）再 **`close`**。

---

## 6. 小结

| 层次 | 职责 |
|------|------|
| **`dev_watchdog.h`** | **WDT 设备类型、`ops` 表、标准 `control` 宏** |
| **`dev_watchdog.c`** | **薄封装：`init/open/close/control` → `ops`** |
| **BSP** | **寄存器操作、时钟、复位行为、是否可停狗** |

该子系统是 RT-Thread **最简单的一类字符设备框架之一**：代码量少、语义清晰，工程复杂度主要集中在 **芯片手册与系统策略（何时喂狗、是否允许关闭）**。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/watchdog/`。*
