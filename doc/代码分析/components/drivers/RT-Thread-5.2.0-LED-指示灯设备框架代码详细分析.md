# RT-Thread 5.2.0 LED 设备框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/led` 目录实现的 **LED 字符设备框架**（`led.c`）及可选的 **设备树 GPIO LED 平台驱动**（`led-gpio.c`）。类型与 API 在 **`components/drivers/include/drivers/led.h`**，`rtdevice.h` 在 **`RT_USING_DM`** 且 **`RT_USING_LED`** 时包含 **`drivers/led.h`**（LED 依赖 DM，见 **`Kconfig`**）。

涉及文件：

- **`led.c`**：注册、**`read`/`write`** 封装、闪烁软件定时器
- **`led-gpio.c`**：**`gpio-leds`** 兼容的 OFW 驱动
- **`Kconfig`、`SConscript`**

---

## 1. 模块定位与依赖

```text
应用 / Shell（write "on" / read 状态字符串）
    ↓
rt_device（RT_Device_Class_Char，名 led%u）
    ↓
rt_led_* → rt_led_ops（set_state / get_state / set_period / set_brightness）
    ↓
GPIO 或其它背光/PMIC 驱动
```

| Kconfig | 含义 |
|---------|------|
| **`RT_USING_LED`** | 总开关；**`depends on RT_USING_DM`** |
| **`RT_LED_GPIO`** | 编译 **`led-gpio.c`**；依赖 **`RT_USING_PINCTRL`**、**`RT_USING_OFW`** |
| **`osource "$(SOC_DM_LED_DIR)/Kconfig"`** | SoC/BSP 可外接额外 LED 相关配置 |

**`SConscript`**：要求 **`RT_USING_DM` && `RT_USING_LED`**；始终编 **`led.c`**；**`RT_LED_GPIO`** 时增加 **`led-gpio.c`**。

---

## 2. 数据结构与操作表（`led.h`）

### 2.1 `enum rt_led_state`

**`OFF` / `ON` / `TOGGLE` / `BLINK`**，以及 **`RT_LED_STATE_NR`**（可用于边界）。

### 2.2 `struct rt_led_device`

- **`parent`**：**`struct rt_device`**
- **`ops`**：**`const struct rt_led_ops *`**
- **`spinlock`**：保护状态机与 **`blink_timer`**
- **`sysdata`**：框架私有；无 **`ops->set_period`** 时指向 **`struct blink_timer`**
- **`priv`**：驱动私有数据

### 2.3 `struct rt_led_ops`

| 回调 | 可选 | 说明 |
|------|------|------|
| **`set_state`** | 建议实现 | 硬件亮灭/切换 |
| **`get_state`** | 可选 | 查询当前 **ON/OFF** 语义 |
| **`set_period`** | 可选 | 硬件或驱动侧闪烁周期（毫秒） |
| **`set_brightness`** | 可选 | 亮度（**`write` 数字串** 会调此接口） |

未实现 **`set_period`** 但实现了 **`set_state`** 时，**`led.c`** 在 **`rt_led_register`** 里分配 **`blink_timer`**，用 **`rt_timer`** 周期调用 **`_led_blink_timerout`** 在 **ON/OFF** 间切换，实现 **软件 BLINK**。

---

## 3. 核心实现（`led.c`）

### 3.1 设备命名与 ID

- **`static struct rt_dm_ida led_ida = RT_DM_IDA_INIT(LED)`**
- **`rt_dm_ida_alloc`** 分配 **`device_id`**，**`rt_dm_dev_set_name(..., "led%u", device_id)`** → 设备名 **`led0`、`led1`** …
- **`parent.master_id` / `device_id`** 写入 **`rt_device`**，便于 DM/调试统一标识

### 3.2 `rt_led_register`

1. 校验 **`led`/`ops`**；**`rt_dm_ida_alloc`** 失败返回 **`-RT_EFULL`**。
2. **`rt_spin_lock_init`**
3. 若 **无 `set_period` 且有 `set_state`**：**`rt_malloc(sizeof(struct blink_timer))`**，**`rt_timer_init(..., PERIODIC, 500ms)`**，超时函数 **`_led_blink_timerout`** 交替 **`set_state(OFF/ON)`**。
4. **`parent.type = RT_Device_Class_Char`**，仅挂 **`read`/`write`**（无 **`open`/`control`**）。
5. **`rt_device_register(..., RT_DEVICE_FLAG_RDWR)`**；失败路径 **`rt_dm_ida_free`**、**`rt_timer_detach`**、**`rt_free(btimer)`**。

### 3.3 `rt_led_unregister`

**`rt_led_set_state(OFF)`**；若有 **`blink_timer`**：**`detach`** 并 **`rt_free`**；**`rt_dm_ida_free`**；**`rt_device_unregister`**。

### 3.4 `rt_led_set_state`

自旋锁内：若 **`blink_timer` 已 `enabled`**，先 **`rt_timer_stop`**。调用 **`ops->set_state`**。

- 若 **`state == RT_LED_S_BLINK`** 且 **`set_state` 返回 `-RT_ENOSYS`** 且存在 **`blink_timer` 且尚未 `enabled`**：置 **`enabled`**，**`rt_timer_start`**（由框架用软件定时器实现 **BLINK**）。
- 其它状态：若 **`blink_timer` 在跑**，成功则 **`enabled = RT_FALSE`**；失败则 **重启定时器**（保持闪烁）。

**语义**：驱动可对 **`BLINK`** 返回 **`ENOSYS`** 以退回到框架软件闪烁；若驱动原生支持 **`BLINK`**，则 **`err == RT_EOK`** 时不会走 **`enabled`** 分支（**`!btimer->enabled`** 条件不满足），由硬件自行闪烁。

### 3.5 `rt_led_set_period`

若 **`ops->set_period`** 存在则转调；否则在 **`blink_timer`** 存在时用 **`RT_TIMER_CTRL_SET_TIME`** 改 **`rt_timer`** 周期。

### 3.6 `rt_led_get_state` / `rt_led_set_brightness`

在自旋锁下调用 **`ops`**；无对应 **`ops`** 返回 **`-RT_ENOSYS`**。

### 3.7 字符设备 **`read` / `write`**

**`_led_read`**：调用 **`rt_led_get_state`**，把枚举映射为 **`"off"`/`"on"`/`"toggle"`/`"blink"`** 子串，支持 **`pos` 偏移** 分段读。

**`_led_write`**：

- 意图：将写入缓冲区与上述字符串比对，匹配则 **`rt_led_set_state`**；否则把内容解析为 **十进制无符号整数**，调用 **`rt_led_set_brightness`**。

**实现问题（源码现状）**：循环里使用 **`rt_strncpy((char *)_led_states[i], buffer, size)`**，会把 **`buffer`** 拷入 **只读静态字符串 `_led_states[i]``**，且 **`rt_strncpy` 返回值** 也不是“是否相等”。此处与常见 **`strncmp`/`memcmp`** 写法不符，**极可能为缺陷**；实际产品上应核对 **`write`** 路径是否按预期工作，必要时在 BSP 侧只用 **`rt_led_*` API** 而避开 **`write` 字符串分支**。

---

## 4. GPIO LED 平台驱动（`led-gpio.c`）

### 4.1 `struct gpio_led`

内嵌 **`struct rt_led_device parent`**，另 **`pin`**、**`active_val`**（高有效或低有效，由 **`rt_ofw_get_named_pin`** 填充）。

### 4.2 `gpio_led_ops`

- **`set_state`**：**`OFF`** 写 **`!active_val`**，**`ON`** 写 **`active_val`**；**`TOGGLE`** 先 **`get_state`** 再反相 **`set_state`**；**`BLINK`** 返回 **`-RT_ENOSYS`**（交给 **`led.c`** 软件定时器）。
- **`get_state`**：按 **`rt_pin_read`** 为 **LOW/HIGH** 映射 **OFF/ON**。

### 4.3 OFW：`ofw_append_gpio_led`

- **`rt_ofw_get_named_pin(np, ...)`** 取 GPIO 与有效电平。
- **`rt_led_register`**。
- **`default-state`**：**`"on"`** 则上电点亮，否则默认灭。
- **`default-trigger$`**（模糊属性名）：若为 **`heartbeat`** 或 **`timer`**，则 **`rt_led_set_state(BLINK)`**。
- **`rt_ofw_data(np) = &gled->parent`** 供 **`remove`** 查找。

### 4.4 `gpio_led_probe` / `remove`

- **compatible**：**`"gpio-leds"`**（与 Linux 一致）。
- 若节点有 **`pinctrl-0`**，对 **pdev** 应用 pinctrl；否则对每个子 LED 节点单独 **`rt_pin_ctrl_confs_apply_by_name`**。
- **`remove`**：遍历子节点，**`rt_ofw_data`** 非空则 **`rt_led_unregister`** 并 **`rt_free(gled)`**。

**导出**：**`RT_PLATFORM_DRIVER_EXPORT(gpio_led_driver)`**。

---

## 5. 使用方式小结

| 方式 | 说明 |
|------|------|
| **DM + DTS** | 板级 **`gpio-leds`** 子节点 + **`RT_LED_GPIO`**，自动 **`probe`** |
| **手动注册** | 静态 **`struct rt_led_device` + `rt_led_ops`**，**`rt_led_register`** |
| **应用** | **`rt_device_find("led0")`** 后 **`write`/`read`**（注意上一节 **`write` 实现风险**），或直接 **`rt_led_set_state((struct rt_led_device *)...)`**（需拿到 **`rt_led_device *`**，通常自定义驱动不导出裸指针时更宜封装 API） |

---

## 6. 小结

| 项目 | 说明 |
|------|------|
| 设备类型 | **`RT_Device_Class_Char`**，设备名 **`led%u`** |
| 闪烁 | 驱动 **`set_period`** 或框架 **`rt_timer` 500ms 默认可改** |
| GPIO 路径 | **`gpio-leds` + pinctrl + default-state/trigger** |
| 依赖 | **DM 必开**；GPIO 路径另需 **PINCTRL、OFW** |

阅读顺序：**`led.h`** → **`led.c`**（**`rt_led_set_state` 与 `blink_timer` 交互**）→ **`led-gpio.c`**（DT 属性）。
