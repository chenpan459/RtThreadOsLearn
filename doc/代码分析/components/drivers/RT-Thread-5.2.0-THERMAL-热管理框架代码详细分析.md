# RT-Thread 5.2.0 Thermal（热区 / 散热设备 / Governor）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/thermal` 目录实现的 **热管理框架**：对齐 Linux **设备树 `thermal-zones` / trips / cooling-maps`** 的解析思路，由 **`struct rt_thermal_zone_device`（温区）** 周期性采样温度，根据 **trip 类型** 触发 **散热 `struct rt_thermal_cooling_device`** 的 **档位调节**；调节策略由 **`struct rt_thermal_cooling_governor` 的 `tuning` 回调** 完成，默认内置 **`dumb`** governor。对外头文件为 **`components/drivers/include/drivers/thermal.h`**；**`rtdevice.h`** 在 **`RT_USING_THERMAL`** 下包含 **`drivers/thermal.h`**。

**依赖**：**`Kconfig` 中 `RT_USING_THERMAL` `depends on RT_USING_DM`**，即热框架与 **设备模型（含 OFW 解析路径）** 强相关；芯片侧可通过 **`osource "$(SOC_DM_THERMAL_DIR)/Kconfig"`** 与 **`$(SOC_DM_THERMAL_COOL_DIR)/Kconfig`** 扩展传感器与散热器驱动。

```1:4:rt-thread-5.2.0/components/drivers/thermal/Kconfig
menuconfig RT_USING_THERMAL
    bool "Using Thermal Management device drivers"
    depends on RT_USING_DM
    default n
```

---

## 1. 目录与编译

| 文件 | 条件 | 作用 |
|------|------|------|
| **`thermal.c`** | **`RT_USING_THERMAL`** | **温区/散热器/governor 注册、OFW 绑定、`rt_thermal_zone_device_update`、工作队列轮询、`list_thermal` MSH** |
| **`thermal_dm.c`** | 同上 | **`thermal_type()` 字符串 → trip 类型、`thermal_bind`/`thermal_unbind` 封装** |
| **`thermal-cool-pwm-fan.c`** | **`RT_THERMAL_COOL_PWM_FAN`**（依赖 **PWM、REGULATOR、OFW**） | **`pwm-fan` 平台驱动**：**`cooling-levels` + `pwms` + 可选 `fan` 供电** |
| **`thermal_dm.h`** | 被 **`thermal.c` / `thermal_dm.c`** 使用 | **DM 侧 bind 与 trip 类型解析声明** |
| **`SConscript`** | — | 未 **`RT_USING_THERMAL`** 则不编；可选追加 **`thermal-cool-pwm-fan.c`** |

---

## 2. 温度单位与绑定常量

- **Trip 温度、当前温区温度**：源码与注释均为 **毫摄氏度（millidegree Celsius）**，例如 **`25000` 表示 25.000 ℃**。
- **`RT_THERMAL_TEMP_INVALID`**：**`-274000`**（无效采样占位）。
- **`RT_THERMAL_NO_LIMIT`**：来自 **`dt-bindings/thermal/thermal.h`** 的 **`THERMAL_NO_LIMIT`（`~0`）**，表示 **无上下限约束** 等语义（与 Linux binding 一致）。

---

## 3. 核心数据结构（`thermal.h`）

### 3.1 `struct rt_thermal_zone_device`（温区）

| 成员 | 含义 |
|------|------|
| **`ops`** | **`get_temp` 必选**；可选 **`set_trips` / `set_trip_temp` / `set_trip_hyst` / `hot` / `critical`** |
| **`trips` / `trips_nr`** | **阈值表**（类型：**active/passive/hot/critical**） |
| **`params`** | **`sustainable-power`、`slope`、`offset`**（OFW **`coefficients`** 前两系数等） |
| **`enabled` / `cooling`** | 是否启用；是否处于 **需散热状态** |
| **`temperature` / `last_temperature`** | 当前与上一次采样 |
| **`prev_low_trip` / `prev_high_trip`** | 与 **`set_trips`** 配合的 **硬件 trip 窗口缓存** |
| **`notifier_nodes` + `nodes_lock`** | **扩展通知链表** |
| **`cooling_maps` / `cooling_maps_nr`** | **本温区与散热设备的映射及档位区间** |
| **`passive_delay` / `polling_delay`** | **工作队列重新调度间隔（tick）** |
| **`poller`（`rt_work`）** | **`thermal_zone_poll` → `rt_thermal_zone_device_update`** |
| **`mutex`** | 温区更新路径 **非中断上下文** 下与 **`get_temp`/notifier** 等协同 |

### 3.2 `struct rt_thermal_cooling_device`（散热器）

- **`ops`**：**`get_max_level` / `get_cur_level` / `set_cur_level` 必选**；可选 **`bind` / `unbind`**。
- **`max_level`**：注册时由 **`get_max_level` 填充**。
- **`gov` + `governor_node`**：当前绑定的 **governor** 及其全局链表节点。

### 3.3 `struct rt_thermal_cooling_governor`

- **`name` + `tuning(zdev, map_idx, cell_idx, *level)`**：根据 **温区状态与 trip** 调整 **目标档位**。

### 3.4 消息位 **`RT_THERMAL_MSG_*`**

用于 **`rt_thermal_zone_device_update(zdev, msg)`** 及 **notifier 回调** 的 **事件分类**（采样、trip 越界/变更、设备上下线等）。

---

## 4. `thermal.c`：注册与 OFW

### 4.1 全局链表

在 **`nodes_lock`** 下维护：

- **`thermal_zone_device_nodes`**：所有 **`rt_thermal_zone_device`**；
- **`thermal_cooling_device_nodes`**：所有 **`rt_thermal_cooling_device`**；
- **`thermal_cooling_governor_nodes`**：所有 **governor**。

### 4.2 `thermal_ofw_setup`（**`RT_USING_OFW`**）

1. 查找 **`/thermal-zones`** 下子节点，匹配 **`thermal-sensors`** phandle 指向 **本传感器节点 `np`**，且 **`#thermal-sensor-cells` 第二参数与 `zdev->zone_id` 一致**（或 **无 cells**）。
2. 读取 **`polling-delay-passive` / `polling-delay`** → **`passive_delay` / `polling_delay`**（**`rt_tick_from_millisecond`**）。
3. **`thermal_ofw_params_parse`**：**`sustainable-power`**、**`coefficients`**（仅取前两个为 **`slope`/`offset`**；注释写明 **当前框架每温区仅一颗传感器**）。
4. **`trips` 子节点**：分配 **`zdev->trips[]`**，填 **temperature、hysteresis、type**，**`rt_ofw_data(trip_np) = &trips[i]`** 供 **cooling-map 的 `trip` phandle** 反查。
5. **`cooling-maps`**：每个 map 读 **`trip`**、**`contribution`**、**`cooling-device` + `#cooling-cells`**；在 **散热器链表** 中按 **`ofw_node` 匹配** **`rt_thermal_cooling_device`**，填 **`level_range[0..1]`**，并 **`thermal_bind(cdev, zdev)`**。

### 4.3 `rt_thermal_zone_device_register`

- 校验 **`ops` 与 `get_temp`**。
- 初始化 **自旋锁、互斥量、`rt_work`**，加入 **全局温区链表**，调用 **`thermal_ofw_setup`**。
- **`enabled = RT_TRUE`**，**`rt_work_submit(&poller, polling_delay)`** 启动轮询。

**注意**：实现中在 **`get_temp` 预热读** 之后又将 **`zdev->temperature` 置为 `RT_THERMAL_TEMP_INVALID`** 再开始调度，**首次展示温度以第一次 `update` 为准**，阅读源码时需留意该顺序。

### 4.4 `thermal_zone_poll`

工作队列回调中调用 **`rt_thermal_zone_device_update(zdev, RT_THERMAL_MSG_EVENT_UNSPECIFIED)`**，形成 **周期性热采样闭环**。

---

## 5. `rt_thermal_zone_device_update`：温控主逻辑

（**非 ISR 嵌套** 时 **`rt_mutex_take(&zdev->mutex)`**。）

1. **设备上下线**：根据 **`msg`** 置 **`enabled`**；**`DEVICE_DOWN`** 时 **`rt_work_cancel`** 停止轮询。
2. **`last_temperature ← temperature`**，再 **`ops->get_temp`** 更新 **`temperature`**。
3. **遍历 trips**，若 **`temperature <= trip->temperature`** 则跳过；否则按 **`type`**：
   - **`PASSIVE`**：置 **`passive`**，进入 **cooling**（**`kick` 调档**）。
   - **`CRITICAL`**：若有 **`ops->critical`** 则调用；否则若 **上次温度已高于 critical** 认为 **散热失败** → **`rt_hw_cpu_reset()`**；否则进入 **cooling**。
   - **`HOT`**：若有 **`ops->hot`** 则调用；否则落入 **default → cooling**。
   - **其它 / default**：**cooling**。
4. 若 **本次不需要 cooling 但 `zdev->cooling` 仍为真**：再 **`kick` 一次**（用于 **降温后回调档位**）。
5. **`set_trips`**：根据当前温度与各 trip **计算 low/high 窗口**，与 **`prev_*` 比较** 避免重复下发，调用 **`ops->set_trips(zdev, low, high)`**（硬件 trip 中断窗口优化）。
6. **Notifier**：在 **`nodes_lock`** 下 **安全遍历**，对每个 **`rt_thermal_notifier` 调用 `callback(notifier, msg)`**。
7. **再次调度 `poller`**：**`passive_delay` 优先于 `polling_delay`**；**`enabled` 为假** 则 **cancel**。

---

## 6. `rt_thermal_cooling_device_kick`

对 **`cooling_maps` 中每个 cell**：

- 取 **`cdev`、`get_max_level` / `get_cur_level`**。
- 若 **当前 `level` 已在 `cell->level_range` 内**：认为 **已在“该 map 定义的冷却工作区间”**，**跳过**（注释：**Is cooling, not call**）。
- 否则：**`cdev->gov->tuning(...)`** 得到新 **`level`**，**clamp 到 `max_level`** 后 **`set_cur_level`**。

因此 **`level_range`** 来自 DT **`#cooling-cells`** 的两个参数，表示 **该 trip 下此散热器应工作的档位区间**；**governor** 在区间外才被调用以 **推入区间内**。

---

## 7. Governor 与 `thermal_dm.c`

- **`INIT_CORE_EXPORT(system_thermal_cooling_governor_init)`** 注册默认 **`dumb`** governor：**温度高于 map 关联 trip 且 `zdev->cooling`** 时，按 **与 `last_temperature` 的差与 `hysteresis` 比较** **加档/减档**；否则 **档位归零**。
- **`rt_thermal_cooling_device_register`** 成功后 **`rt_thermal_cooling_device_change_governor(cdev, RT_NULL)`** → **默认绑定 `dumb`**。
- **`thermal_bind` / `thermal_unbind`**：若 **`cdev->ops` 提供 bind/unbind** 则调用，否则 **直接 `RT_EOK`**。

---

## 8. `thermal-cool-pwm-fan.c`（`pwm-fan`）

- **`compatible = "pwm-fan"`**，**`RT_PLATFORM_DRIVER_EXPORT`**。
- **DT**：**`pwms`**（解析 **`rt_device_pwm` + channel + period**）、**`cooling-levels`**（每项 **0–255 占空映射到 pulse**）、可选 **`fan` regulator**。
- **`set_cur_level`**：**非 0 档** 按表写 **PWM pulse** 并在 **从 0 抬起时 `pwm_enable` + `regulator_enable`**；**0 档** **`pwm_disable` + `regulator_disable`**。
- **`max_level = levels_nr - 1`**；上电后先 **`set_cur_level(0)`** 将风扇置于 **关/最低档**。

---

## 9. 其它 API 与 MSH

- **`rt_thermal_zone_set_trip` / `get_trip`**：在 **mutex** 下更新 **trip 或调用 HAL**，成功后 **`update(..., TRIP_CHANGED)`**。
- **`rt_thermal_zone_notifier_register/unregister`**：挂接 **自定义策略或上报**。
- **`list_thermal`**：**MSH** 导出，打印各温区 **℃** 与各 **cooling map 当前 level**。

---

## 10. 实现细节提示（阅读源码时核对）

1. **`rt_thermal_zone_device_unregister`** 开头对 **`notifier_nodes` 是否为空的判断** 与 **日志“there is %u user”** 组合在一起，**语义上易与“存在 notifier 则不可注销”相反**，集成注销逻辑时建议 **对照当前 `thermal.c` 行号自行验证**。
2. **`rt_thermal_zone_device_unregister` 释放 `cooling_maps` 的内层循环** 中，有一处使用 **`map->cells[i].cooling_devices`** 而循环变量为 **`c`**，**疑似笔误**（应以 **`cells[c]`** 为准），若需长期稳定 **可自行打补丁**。
3. **`rt_thermal_cooling_governor_unregister`**：若 **`gov->cdev_nodes` 非空** 则 **不移除链表**（**无错误码返回**，需调用方保证 **无绑定 cdev**）。

---

## 11. 小结

| 组件 | 职责 |
|------|------|
| **`thermal.h`** | **温区 / 散热器 / governor / notifier / 消息** 数据结构与 API |
| **`thermal.c`** | **全局管理、OFW 解析、`update`/`kick`、轮询 work、dumb governor** |
| **`thermal_dm.c`** | **DT trip 类型字符串、bind 封装** |
| **`thermal-cool-pwm-fan.c`** | **通用 PWM 风扇散热设备** |

该子系统在 **SMP/高负载** 场景下提供 **与 Linux DT 接近的热描述能力**；**SoC 热传感器驱动** 通常在 **`SOC_DM_THERMAL_DIR`** 中实现 **`rt_thermal_zone_ops`** 并 **`rt_thermal_zone_device_register`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/thermal/`。*
