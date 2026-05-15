# RT-Thread 5.2.0 Regulator（电压/电流调节器）框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/regulator` 目录实现的 **Regulator 子系统**：以 **`struct rt_regulator_node`** 描述单路电源节点（**ops + 参数 + 父子树**），对外通过 **`struct rt_regulator` 句柄** 提供 **`rt_regulator_enable`/`set_voltage`** 等 API；内置 **设备树属性解析**、**`regulator-fixed`** 与 **`regulator-gpio`** 两类平台驱动。头文件为 **`components/drivers/include/drivers/regulator.h`**；**`rtdevice.h`** 在 **`RT_USING_REGULATOR`** 下包含 **`drivers/regulator.h`**。

---

## 1. 目录与编译

| 文件 | 条件 | 作用 |
|------|------|------|
| **`regulator.c`** | **`RT_USING_REGULATOR`** | 核心：**注册/注销**、**enable/disable 与父级级联**、**notifier**、**`rt_regulator_get/put`** 与 **`rt_ref`** |
| **`regulator_dm.c`** | 同上 | **`regulator_ofw_parse`**：从 OFW 节点读 Linux 风格 regulator 属性 |
| **`regulator-fixed.c`** | **`RT_REGULATOR_FIXED`** | **`compatible = "regulator-fixed"`** 平台驱动 |
| **`regulator-gpio.c`** | **`RT_REGULATOR_GPIO`** | **`compatible = "regulator-gpio"`** 平台驱动 |
| **`regulator_dm.h`** | 被 **`.c`** 包含 | **`regulator_ofw_parse`** 声明（见下文 **无 OFW 注意点**） |

**`SConscript`**：无 **`RT_USING_REGULATOR`** 则不编译；**`RT_REGULATOR_FIXED`** / **`RT_REGULATOR_GPIO`** 由 **`Kconfig`** 控制追加。

---

## 2. Kconfig 与依赖

| 选项 | 含义 |
|------|------|
| **`RT_USING_REGULATOR`** | 总开关；**`depends on RT_USING_DM`**；**`select RT_USING_ADT`、`RT_USING_ADT_REF`** |
| **`RT_REGULATOR_FIXED`** | 固定电压 + **使能脚**；**`depends on RT_USING_PIN、RT_USING_PINCTRL`**（probe 里 **`rt_pin_ctrl_confs_apply`**） |
| **`RT_REGULATOR_GPIO`** | **多 GPIO 分档调压**；**`depends on RT_USING_PIN`** |
| **`osource "$(SOC_DM_REGULATOR_DIR)/Kconfig"`** | SoC 可扩展更多 regulator 类型 |

---

## 3. 数据模型（`regulator.h`）

### 3.1 `struct rt_regulator_param`

- **电压/电流范围**：**`min_uvolt`/`max_uvolt`/`min_uamp`/`max_uamp`**（微伏、微安）。
- **时序**：**`ramp_delay`、`enable_delay`、`off_on_delay`**（注释单位分别为 **uV/µs**、**µs**）。
- **位域标志**：**`enable_active_high`、`boot_on`、`always_on`、`soft_start`、`pull_down`、`over_current_protection`**。

**`RT_REGULATOR_UVOLT_INVALID`**：无效电压哨兵值。

### 3.2 `struct rt_regulator_node`

- **`list`**：挂入 **父节点** **`children_nodes`**（在 **`regulator_check_parent`** 成功解析 **`vin-supply`** 后 **`rt_list_insert_after`**）。
- **`children_nodes`**：子 regulator 链表头。
- **`dev`**：所属 **`struct rt_device`**（DM 设备）。
- **`parent`**：上游电源 **`rt_regulator_node *`**（**enable/set_voltage` 递归向上**）。
- **`supply_name`**：逻辑名（如 DT **`regulator-name`**）。
- **`ops`**：**`struct rt_regulator_ops`**。
- **`ref`**：**`rt_ref`**，与 **`rt_regulator_put`** 中 **`rt_ref_put(..., regulator_release)`** 配合，引用归零时可 **`rt_regulator_unregister`**。
- **`enabled_count`**：**原子计数**，在 **`regulator_enable`** 成功路径上 **`rt_atomic_add(..., 1)`**。
- **`notifier_nodes`**：回调链表。
- **`priv`**：具体驱动私有数据（本目录 **fixed/gpio** 用 **container_of** 从嵌入的 **`parent`** 反查私有结构）。

### 3.3 `struct rt_regulator`（仅 **`regulator.c` 内可见**）

头文件注释说明：**有意不把 `struct rt_regulator` 定义暴露在 `regulator.h`**，避免非框架代码直接改 **`reg_np`**；应用仅通过 **指针** 调用 **`rt_regulator_*` API**。

### 3.4 `struct rt_regulator_ops`

**`enable`/`disable`/`is_enabled`/`set_voltage`/`get_voltage`/`set_mode`/`get_mode`/`set_ramp_delay`/`enable_time`** — 除 **`get_voltage`** 在 **`regulator_set_voltage` 通知链** 中被 **`RT_ASSERT` 要求非空** 外，其余均可选（未实现则上层返回 **`-RT_ENOSYS`** 或走 **`enabled_count` 推断**）。

### 3.5 Notifier

- **消息位**：**`RT_REGULATOR_MSG_ENABLE/DISABLE/VOLTAGE_CHANGE/VOLTAGE_CHANGE_ERR`**。
- **`union rt_regulator_notifier_args`**：**调压前后** 的 **`old_uvolt`/`min_uvolt`/`max_uvolt`**。

---

## 4. `regulator.c`：核心逻辑

### 4.1 `rt_regulator_register`

- 校验 **`reg_np`/`dev`/`param`/`ops`**。
- 初始化 **链表头、`rt_ref`、`enabled_count=0`**。
- **OFW**：若 **`dev->ofw_node`** 存在，**`rt_ofw_data(node) = reg_np`**，供 **`rt_regulator_get`** 通过 phandle 解析。
- **`boot_on` 或 `always_on`**：调用内部 **`regulator_enable(reg_np)`** 上电。

**注意**：**`rt_regulator_register` 不调用 `regulator_check_parent`**；**`parent` 与 `children_nodes` 挂链** 在 **`rt_regulator_get`** 里首次 **`regulator_check_parent`** 时完成。

### 4.2 `rt_regulator_unregister`

在锁内检查：**`enabled_count != 0`**、**子节点非空** 或 **`rt_ref_read > 1`** 则 **`-RT_EBUSY`**；否则非 **boot_on/always_on** 时 **`regulator_disable`**，**`rt_list_remove(&reg_np->list)`**（从父 **`children_nodes`** 摘除）。

### 4.3 使能链 **`regulator_enable` / `regulator_disable`**

- **`regulator_enable`**：若 **`ops->enable`** 成功，则 **`regulator_delay(enable_delay)`**（来自 **`param->enable_delay`** 或 **`ops->enable_time`**），**`enabled_count++`**，走 **notifier `MSG_ENABLE`**，再 **`regulator_enable(parent)`**（**先本节点后父 supply**）。
- **`regulator_disable`**：对称：**`ops->disable`** → **`off_on_delay`** → **notifier `MSG_DISABLE`** → **`regulator_disable(parent)`**。

### 4.4 对外 **`rt_regulator_enable` / `rt_regulator_disable`**

- **`rt_regulator_enable`**：若 **`rt_regulator_is_enabled`** 已为真则直接 **`RT_EOK`**；否则加锁调用 **`regulator_enable`**。

- **`rt_regulator_disable`**（阅读源码时建议对照计数与硬件状态）：

```306:333:rt-thread-5.2.0/components/drivers/regulator/regulator.c
rt_err_t rt_regulator_disable(struct rt_regulator *reg)
{
    rt_err_t err;

    if (!reg)
    {
        return -RT_EINVAL;
    }

    if (!rt_regulator_is_enabled(reg))
    {
        return RT_EOK;
    }

    if (rt_atomic_load(&reg->reg_np->enabled_count) != 0)
    {
        rt_atomic_sub(&reg->reg_np->enabled_count, 1);

        return RT_EOK;
    }

    rt_hw_spin_lock(&_regulator_lock.lock);

    err = regulator_disable(reg->reg_np);

    rt_hw_spin_unlock(&_regulator_lock.lock);

    return err;
}
```

在 **`enabled_count != 0`** 时仅 **原子减一** 并返回，**不调用** **`regulator_disable`**；仅当 **`enabled_count` 已为 0** 时才进入 **`regulator_disable`**。与 **`regulator_enable` 成功时对 `enabled_count` 加一** 组合后，**一次 `rt_regulator_enable` 后第一次 `rt_regulator_disable` 往往只递减计数而不关断硬件**；**再次** **`rt_regulator_disable`**（且 **`is_enabled` 仍为真**）才会真正 **`ops->disable`**。集成电源驱动时务必 **用示波器/逻辑或单测** 验证是否符合预期，必要时本地调整逻辑。

### 4.5 **`rt_regulator_is_enabled`**

若存在 **`ops->is_enabled`** 则调用之；否则 **`enabled_count > 0`**。**`!reg` 时返回 `-RT_EINVAL`**（类型为 **`rt_bool_t`**，属于 **API 返回值不严谨** 的隐患）。

### 4.6 调压 **`regulator_set_voltage` / `rt_regulator_set_voltage`**

- 调 **`ops->set_voltage`** 前 **`RT_ASSERT(ops->get_voltage != NULL)`**，先 **`MSG_VOLTAGE_CHANGE` notifier**，成功再 **`set_voltage`**；失败发 **`MSG_VOLTAGE_CHANGE_ERR`**。
- 成功后 **`regulator_set_voltage(parent, ...)`** 同步父级（**整条 supply 链电压语义由驱动约定**）。

### 4.7 **`rt_regulator_get(dev, id)`**

- **OFW**：在 **`dev->ofw_node`** 上读 **`"{id}-supply"`** phandle，**`rt_ofw_find_node_by_phandle`** → **`rt_platform_ofw_request`** 延迟 probe → **`reg_np = rt_ofw_data(np)`**。
- 加锁 **`regulator_check_parent(reg_np)`**：沿 **`vin-supply`** 向上找父 **`rt_regulator_node`**（**`rt_ofw_data`**），并把本节点 **`list`** 挂入父 **`children_nodes`**。
- **`rt_calloc`** 分配 **`struct rt_regulator`**，**`rt_ref_get(&reg_np->ref)`**。

### 4.8 **`rt_regulator_put`**

**`rt_ref_put`**；**`rt_free(reg)`**。当 **引用计数归零** 时 **`regulator_release` → `rt_regulator_unregister`**。

### 4.9 **`regulator_delay`**

混合 **`rt_thread_mdelay` / `rt_hw_us_delay`** 与 **短延时 busy-wait** 策略，供 **上电/掉电时序** 使用。

---

## 5. `regulator_dm.c`：OFW 属性解析

**`regulator_ofw_parse(np, param)`** 读取（与 Linux DT 对齐的）常用属性：

- **`regulator-name`**（**`rt_ofw_prop_read_raw`**）
- **`regulator-min/max-microvolt`、`regulator-min/max-microamp`**
- **`regulator-ramp-delay`、`regulator-enable-ramp-delay`**
- **布尔**：**`enable-active-high`、`regulator-boot-on`、`regulator-always-on`** 等

未读到的字段保持 **`param` 调用前** 的内存内容，**调用方应 `rt_memset`/`rt_calloc` 清零**（**`regulator-fixed`/`gpio` probe** 已 **`rt_calloc`**）。

---

## 6. `regulator-fixed.c`：固定电压 + 使能 GPIO

- **`struct regulator_fixed`**：**`struct rt_regulator_node parent`** 为首成员，内嵌 **`param`** 与 **`enable_pin`**。
- **`enable`/`disable`**：**`rt_pin_mode` + `rt_pin_write`**；无使能脚或 **`always_on`** 则 **空操作成功**。
- **`is_enabled`**：读 **GPIO 电平** 与 **`enable_active_high`** 比对。
- **`get_voltage`**：**`(min + max) / 2` 偏向中点**（若 min==max 即标称值）。
- **`probe`**：**`regulator_ofw_parse`** → **`rt_pin_get_named_pin(dev, "enable", 0, ...)`**，失败再尝试 **`propname == NULL`** 的 **`gpios`**；**`rt_pin_ctrl_confs_apply(dev, 0)`**；可选 **`startup-delay-us`/`off-on-delay-us`**（**DM 属性**）；**`rt_regulator_register`**。

**`INIT_SUBSYS_EXPORT(regulator_fixed_register)`** 注册 **`rt_platform_driver`**。

---

## 7. `regulator-gpio.c`：GPIO 分档调压

- **`states`**：设备树 **`states`** 属性为 **`(uV, gpio_bitmask)`** 对，表示 **每种输出组合对应的电压**。
- **`pins_desc`**：**`gpios`** 解析出多根 **`pin`** + **`gpios-states`** 给出的 **高/低有效极性**（**`PIND_OUT_HIGH`/`PIND_OUT_LOW`**，来自 **`dt-bindings/pin/state.h`**）。
- **`set_voltage`**：在 **[min_uvolt, max_uvolt]** 内选 **满足条件的、电压值最小（代码变量名 `best_val` 初始化 `INVALID`，比较条件为 `state->value < best_val`）** 的状态，再按 **位掩码 `target`** 写各 **GPIO**；并保存 **`rg->state`**。
- **`get_voltage`**：按当前 **`rg->state`** 反查 **`states`** 表。
- **`enable`/`disable`**：可选 **`enable` GPIO**；**`always_on`** 时跳过。

**`is_enabled`**：将 **`enable_active_high ? PIN_LOW : PIN_HIGH`** 记为 **`active_val`**，读脚后与 **`active_val`** 比较——与 **`enable` 写入高有效为高电平** 的常规语义 **易不一致**，建议与硬件原理图及 **`regulator-fixed`** 行为 **交叉验证**。

---

## 8. `regulator_dm.h` 与无 OFW 构建

当前仓库中 **`#else`（非 `RT_USING_OFW`）** 分支写成：

```c
rt_inline rt_err_t regulator_ofw_parse(...);
{
    return RT_EOK;
}
```

**分号后单独花括号在 C 中非法**。实际产品通常 **开启 `RT_USING_OFW` 且使用 fixed/gpio 驱动** 不会触发该分支；若需在 **无 OFW** 下包含此头文件，应 **修正为完整 `inline` 函数体** 或提供 **空桩 `.c`**。

---

## 9. 使用流程小结

```text
驱动 probe → 填充 rt_regulator_node + param + ops
         → rt_regulator_register（OFW 节点挂 reg_np）
消费驱动   → rt_regulator_get(dev, "vio")   // 读 vio-supply
         → rt_regulator_enable / set_voltage
         → rt_regulator_put
```

**`vin-supply`** 在 **`rt_regulator_get`** 时解析，用于 **自动级联 enable/disable**。

---

## 10. 小结

| 层次 | 职责 |
|------|------|
| **`regulator.h`** | 对外 API、**`rt_regulator_node`/`ops`/`param`/notifier** |
| **`regulator.c`** | **引用计数、父子树、通知、get/put** |
| **`regulator_dm.c`** | **DT 通用参数解析** |
| **`regulator-fixed.c`** | **固定输出 + 使能脚 + pinctrl** |
| **`regulator-gpio.c`** | **多脚位编码电压档** |

该子系统 **不实现 PMIC I2C/SPI 协议**；复杂芯片需在 **`SOC_DM_REGULATOR_DIR`** 或其它组件中 **自写 `rt_regulator_ops` 驱动** 并 **`rt_regulator_register`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/regulator/`。*
