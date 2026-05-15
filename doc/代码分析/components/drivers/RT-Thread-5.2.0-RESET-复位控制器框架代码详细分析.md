# RT-Thread 5.2.0 Reset（复位控制器）框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/reset` 目录实现的 **Reset Controller 子系统**：对齐 Linux 设备树 **`resets` / `#reset-cells` / `reset-names`** 语义，由 **`struct rt_reset_controller`** 表示 **复位 IP 硬件模块**，由 **`struct rt_reset_control`** 表示 **消费者设备占用的一路（或多路数组）复位线**；通过 **`rt_reset_control_assert` / `deassert` / `reset`** 驱动外设复位时序。对外头文件为 **`components/drivers/include/drivers/reset.h`**；**`rtdevice.h`** 在 **`RT_USING_RESET`** 下包含 **`drivers/reset.h`**。

---

## 1. 目录与编译

| 文件 | 条件 | 作用 |
|------|------|------|
| **`reset.c`** | **`RT_USING_RESET`** | 控制器注册、**OFW 解析 `resets`**、**`rt_reset_control_*` API**、**数组型 reset 聚合** |
| **`reset-simple.c`** | **`RT_RESET_SIMPLE`** | **MMIO 位操作** 类 **simple reset** 平台驱动（多 SoC **compatible**） |
| **`reset-simple.h`** | 被 **`reset-simple.c`** 及 SoC 扩展使用 | **`struct reset_simple`**、**`reset_simple_ops`** 声明 |
| **`Kconfig`** | — | **`RT_USING_RESET`**、**`RT_RESET_SIMPLE`**、**`SOC_DM_RESET_DIR` 扩展** |
| **`SConscript`** | — | 无 **`RT_USING_RESET`** 则不编译；**`RT_RESET_SIMPLE`** 追加 **`reset-simple.c`** |

**依赖**：**`RT_USING_RESET`** 在 **`Kconfig`** 中 **`depends on RT_USING_DM` 与 `RT_USING_OFW`** — 复位框架 **强依赖设备模型 + 设备树**。

---

## 2. Kconfig

| 选项 | 含义 |
|------|------|
| **`RT_USING_RESET`** | 总开关（默认 **n**） |
| **`RT_RESET_SIMPLE`** | 编译 **通用 simple MMIO** 驱动（默认 **n**） |
| **`osource "$(SOC_DM_RESET_DIR)/Kconfig"`** | 芯片/BSP 可追加 **自有 reset 控制器** Kconfig |

---

## 3. 数据结构与操作表（`reset.h`）

### 3.1 `struct rt_reset_controller`

- **`struct rt_object parent`**：对象名固定为 **`RT_RESET_CONTROLLER_OBJ_NAME`**（**`"RSTC"`**），供 **`rt_ofw_parse_object`** 与 OFW 核心 **`#reset-cells` 映射表** 匹配（见 **`components/drivers/ofw/ofw.c`** 中 **`#reset-cells` → `sizeof(struct rt_reset_controller)`** 一类元数据）。
- **`rstc_nodes`**：**本控制器上已分配的 `rt_reset_control` 链表**（**`rt_reset_control_put` 时摘除**）。
- **`ops`**：**`struct rt_reset_control_ops`**。
- **`ofw_node`**：控制器 **DT 节点**；**`rt_reset_controller_register`** 时若 **`rt_ofw_data` 为空** 则 **`rt_ofw_data(node) = rstcer`**。
- **`spinlock`**：**`rstc_nodes` 链表与 `put` 并发** 保护。

### 3.2 `struct rt_reset_control`

- **`rstcer`**：所属控制器。
- **`id`**：默认取 **`resets` phandle 解析结果 `args[0]`**（**`#reset-cells` 第一参数**；完整语义可由 **`ops->ofw_parse`** 覆盖写入）。
- **`con_id`**：来自 **`reset-names`** 的 **consumer 侧名称**（可选）。
- **`is_array`**：是否为 **聚合句柄**（见 **`reset.c`** 中 **`reset_control_array`**）。
- **`list`**：挂入 **`rstcer->rstc_nodes`**。

### 3.3 `struct rt_reset_control_ops`

| 成员 | 含义 |
|------|------|
| **`ofw_parse`** | 解析 **`struct rt_ofw_cell_args`**（注释约定 **`args[0]` 常为线号/id**）。 |
| **`reset`** | **脉冲复位**：典型实现 **assert → 延时 → deassert**。 |
| **`assert` / `deassert`** | **拉复位 / 释放复位**（电平语义由驱动定义）。 |
| **`status`** | 查询当前是否处于 **asserted** 等状态；未实现返回 **`-RT_ENOSYS`**。 |

---

## 4. `reset.c`：核心逻辑

### 4.1 `rt_reset_controller_register` / `unregister`

- **`register`**：设置 **`parent.name = "RSTC"`**，初始化 **链表与自旋锁**；若 **`ofw_node`** 已存在且 **`rt_ofw_data` 为空**，则绑定 **`rt_ofw_data = rstcer`**。
- **`unregister`**：在锁内若 **`rstc_nodes` 非空** 返回 **`-RT_EBUSY`**；**不主动清空 `rt_ofw_data`**（与 **clk** 等子系统风格一致，由驱动生命周期管理）。

### 4.2 `rt_reset_control_reset` / `assert` / `deassert`

- **`NULL` 指针** 对 **`reset`/`assert`/`deassert`** 视为 **空操作成功**（**`RT_EOK`**）；**`status(NULL)`** 返回 **`RT_EOK`**（与 **`-RT_ENOSYS`** 区分注意）。
- 若 **`rstc->is_array`**：先对 **captain** 调 **`ops`**，再 **递归** 对 **`reset_control_array->rstcs[]`** 逐项调用；**`assert`/`deassert` 失败路径** 带 **尽力回滚**（**`deassert`/`assert` 逆操作 + 子项逆序恢复**）。

### 4.3 `rt_reset_control_put`

在 **`rstcer->spinlock`** 下 **`rt_list_remove`**，再 **`reset_free`**：若 **array** 则 **逐项 `put` 子句柄** 后 **`rt_free`**。

### 4.4 OFW 获取：`ofw_get_reset_control`

**参数**：**`np`**（消费者节点）、**`index`**（**`resets` 列表下标**）、**`name`**、**`is_array`**。

**数组模式**（**`is_array == RT_TRUE`**）：

1. **`rstc_nr = rt_ofw_count_phandle_cells(np, "resets", "#reset-cells")`**。
2. **`rstc_arr->count = rstc_nr - 1`**，为 **`rstcs[0..count-1]`** 分别调用 **`ofw_get_reset_control(np, i+1, NULL, FALSE)`** — 即 **子句柄对应 DT 中第 1..rstc_nr-1 项**。
3. **`captain`** 的 **`rstc`** 指向 **`&rstc_arr->captain`**，**`is_array = RT_TRUE`**，随后 **公共路径** 用 **`index`（数组入口为 0）** 再 **`rt_ofw_parse_phandle_cells(np, "resets", #reset-cells, 0, ...)`** — **captain 绑定列表第 0 项 phandle 对应的控制器**，**`ops` 调用顺序**为先 **captain** 再 **子项**。

**单路模式**：**`rt_calloc` 单个 `rt_reset_control`**。

**解析 phandle 后**：

- **`rt_platform_ofw_request(reset_np)`** 若控制器尚未 probe。
- **`rt_ofw_parse_object(reset_np, RT_RESET_CONTROLLER_OBJ_NAME, "#reset-cells")`** 得到 **`struct rt_object *`**，再 **`container_of` → `struct rt_reset_controller *`**。
- **`reset-names`**：若 **`name == NULL`** 且节点存在 **`reset-names`**，则 **`rt_ofw_prop_read_string_index(np, "reset-names", index, &name)`** 填 **`con_id`**。
- **`rstcer->ops->ofw_parse`** 存在则调用，失败则 **`_fail`**。
- **默认 `rstc->id = reset_args.args[0]`**。
- **`rt_list_insert_after(&rstcer->rstc_nodes, &rstc->list)`**。

**对外封装**：

- **`rt_ofw_get_reset_control_array(np)`** → **`ofw_get_reset_control(np, 0, NULL, RT_TRUE)`**。
- **`rt_ofw_get_reset_control_by_index(np, index)`** → **`is_array = RT_FALSE`**。
- **`rt_ofw_get_reset_control_by_name`**：用 **`reset-names`** 查 **index** 再 **`ofw_get_reset_control(np, index, name, RT_FALSE)`**。

**设备侧便捷接口**：**`rt_reset_control_get_*`** 直接转发到 **`dev->ofw_node`** 的 **`rt_ofw_get_*`**。

**注意**：**`ofw_get_reset_control` 在 `_fail` 时** 对 **非 array** 会 **`rt_free(rstc)`**；**array 分配失败**路径会 **释放已创建的子句柄**。

---

## 5. `reset-simple.c`：MMIO 位带 Simple 驱动

### 5.1 `struct reset_simple`（`reset-simple.h`）

内嵌 **`struct rt_reset_controller parent`**，并包含：

- **`mmio_base`**：寄存器映射基址（**`rt_dm_dev_iomap(dev, 0)`**）。
- **`active_low`**：**置位为 1 表示 deassert 还是 assert**（与 **`reset_simple_update`** 中 **`assert ^ active_low`** 异或逻辑配合）。
- **`status_active_low`**：**读回位与“处于复位态”的极性关系**。
- **`reset_us`**：**`reset` 脉冲** 在 assert/deassert 之间的 **微秒延时**；**0** 表示 **`reset` 操作不支持**（**`-RT_ENOSYS`**）。
- **`lock`**：**RMW** 保护（控制器级，与 **`parent.spinlock`** 不同层）。

### 5.2 寄存器布局

**`reset_simple_update`**：按 **32 位 bank** 计算 **`bank = id / 32`**、**`offset = id % 32`**，**读-改-写** **`HWREG32(mmio_base + bank*4)`** 的对应 **bit**。

### 5.3 `reset_simple_ops`

- **`assert` / `deassert`**：调 **`reset_simple_update(..., RT_TRUE/RT_FALSE)`**。
- **`reset`**：若 **`reset_us == 0`** 返回 **`-RT_ENOSYS`**；否则 **assert → `rt_hw_us_delay(reset_us + reset_us/2)` → deassert**。
- **`status`**：**`!(value & bit) ^ !status_active_low`**，把 **位值** 映射为 **逻辑“是否 assert”**。

**未实现 `ofw_parse`**：消费者 **`id` 完全依赖 `args[0]`**。

### 5.4 `reset_simple_probe` 流程

1. **`rt_calloc`** **`struct reset_simple`**。
2. **`rt_dm_dev_iomap(dev, 0)`**。
3. **`rstcer = &rsts->parent`**：**`priv`、`ofw_node`、`ops = &reset_simple_ops`**。
4. **`rt_reset_controller_register(rstcer)`**。
5. 若 **`pdev->id->data`**（**`struct reset_simple_data *`**）非空：**`mmio_base += reg_offset`**，并覆盖 **`active_low`/`status_active_low`**。

**`compatible` 表**（节选）：**`altr,stratix10-rst-mgr`**（带 **reg_offset 0x20**）、**`st,stm32-rcc`**、**`allwinner,sun6i-a31-clock-reset`**、**Aspeed LPC reset**、**DesignWare high/low reset**、**Sophgo SG2042** 等，通过 **`.data`** 区分 **极性/偏移**。

**`INIT_SUBSYS_EXPORT(reset_simple_register)`** 注册 **`rt_platform_driver`**。

---

## 6. 与消费者驱动集成（典型用法）

```c
struct rt_reset_control *rstc;

rstc = rt_reset_control_get_by_name(dev, "eth");
/* 或 rt_reset_control_get_by_index(dev, 0); */

rt_reset_control_assert(rstc);
rt_reset_control_deassert(rstc);
/* 或 rt_reset_control_reset(rstc); */

rt_reset_control_put(rstc);
```

设备树侧示例（概念）：**`resets = <&rst 5>, <&rst 6>;`** **`* #reset-cells = <1>;`**，可选 **`reset-names = "core", "phy"`**。

---

## 7. 与 OFW 核心的衔接

**`components/drivers/ofw/ofw.c`** 将 **`#reset-cells`** 与 **`RT_RESET_CONTROLLER_OBJ_NAME`**、**`sizeof(struct rt_reset_controller)`** 关联，使 **`rt_ofw_parse_object`** 能校验 **phandle 目标节点** 是否为 **已注册的 reset 控制器对象**。控制器驱动须在 **probe 完成硬件初始化** 后调用 **`rt_reset_controller_register`**，消费者 **`rt_platform_ofw_request`** 才能解析 **`resets`**。

---

## 8. 小结

| 层次 | 职责 |
|------|------|
| **`reset.h`** | **控制器 / 控制句柄 / ops** 与 **get/put API** |
| **`reset.c`** | **DT phandle 解析**、**链表管理**、**数组聚合与错误回滚** |
| **`reset-simple.c`** | **常见 SoC 的 MMIO 单 bit 复位** 参考实现 |

**扩展**：SoC 特有 **RCC/CRU** 若与 **simple 位域模型** 不符，应在 **`SOC_DM_RESET_DIR`** 增加 **独立 `rt_platform_driver`**，实现 **`rt_reset_control_ops`**（可带 **`ofw_parse`** 解析多 cell），并 **`rt_reset_controller_register`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/reset/`。*
