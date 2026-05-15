# RT-Thread 5.2.0 CLK 通用时钟框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/clk` 目录实现的 **Common Clock Framework（CCF）风格** 时钟核心，以及与设备树 **`fixed-clock`** 对接的内建驱动。SoC 专用 PLL/MUX/GATE 等通常在 **`$(SOC_DM_CLK_DIR)`**（由 `Kconfig` 的 **`osource`** 引入）中扩展，本目录提供**公共基础设施**。

涉及文件：

- 核心：`clk.c`
- 固定频率时钟（OFW）：`clk-fixed-rate.c`
- 对外 API：`components/drivers/include/drivers/clk.h`（`rtdevice.h` 在 **`RT_USING_CLK`** 下包含）
- 配置与构建：`Kconfig`、`SConscript`

---

## 1. 模块定位与依赖

| 依赖 | 说明 |
|------|------|
| **`RT_USING_DM`** | `Kconfig` 中 `RT_USING_CLK` **depends on** 设备模型 |
| **`RT_USING_ADT_REF`** | `select RT_USING_ADT_REF`，节点使用 **`struct rt_ref`** 做引用计数 |
| **`RT_USING_OFW`** | 可选；开启时额外编译 **`clk-fixed-rate.c`**，并在 **`clk.c`** 中启用 **`rt_ofw_get_clk*`** 等实现 |

**`clk.h`** 还包含 **`drivers/ofw.h`**，时钟解析与 phandle 强相关。

---

## 2. 核心概念：`rt_clk_node` 与 `rt_clk`

### 2.1 `struct rt_clk_node`（时钟提供者 / 硬件节点）

- 内嵌 **`struct rt_object rt_parent`**，对象名宏 **`RT_CLK_NODE_OBJ_NAME`**（`"CLKNP"`），便于在 OFW 中 **`rt_ofw_parse_object`** 找到对应 **`rt_clk_node`**。
- **`list`**：挂入全局 **`_clk_nodes`**（无父节点时）或父节点的 **`children_nodes`**。
- **`children_nodes`**：子节点链表头。
- **`name` / `ops` / `parent`**：节点名、**`rt_clk_ops`**、父时钟节点指针。
- **`ref`**：**`rt_ref`**，**`clk_get`/`clk_put`** 维护生命周期；**`clk_release`** 当前实现仅打日志并 **`RT_ASSERT(0)`**，即**引用降到 0 的完整释放路径尚未实现**，依赖方应避免错误平衡 `get/put`。
- **`rate` / `min_rate` / `max_rate`**：当前速率及允许范围（**`rt_clk_set_rate_range`** 等会修改上下限）。
- **`notifier_count`**：已注册 notifier 数量统计。
- **`priv`**：驱动私有数据。
- **`clk`**：与本硬件节点绑定的 **`struct rt_clk*`**（在 **`rt_clk_register(..., parent_np)`** 且 **`parent_np != NULL`** 时 **`clk_alloc`** 分配；**根节点无父时保持为 `NULL`**，由 **`rt_ofw_get_clk`** 等 **`clk_create`** 再与消费侧关联）。
- **`multi_clk`**：单 OFW 节点输出多路时钟时，可为 **`rt_clk_node` 数组** 的首元素，`rt_ofw_count_of_clk` 会解析 **`clock-indices` / `clock-output-names`** 等并缓存数量。

### 2.2 `struct rt_clk`（消费侧时钟句柄）

表示“某设备从某节点拿到的一路时钟”，典型字段：

- **`clk_np`**：指向提供者 **`rt_clk_node`**。
- **`dev_id` / `con_id`**：设备与连接名（OFW 解析时填入）。
- **`rate`**：若非 0，**`rt_clk_get_rate`** 优先返回该值；否则回落到 **`clk_np->rate`**。
- **`prepare_count` / `enable_count`**：软件引用计数，与 **`prepare`/`enable`** 递归配对。
- **`fw_node` / `priv`**：固件节点指针与私有数据。

### 2.3 `struct rt_clk_ops`

| 回调 | 含义 |
|------|------|
| `init` / `finit` | **`clk_create`** 时 **`init(clk, fw_data)`**；**`rt_clk_put` → clk_free** 时 **`finit`** |
| `prepare` / `unprepare` | 非原子慢路径（可能睡眠）；**`RT_DEBUG_NOT_IN_INTERRUPT`** 约束在 **`rt_clk_prepare`** |
| `is_prepared` / `is_enabled` | 可选查询 |
| `enable` / `disable` | 硬件门控/使能 |
| `set_rate(clk, rate, parent_rate)` | 改频；**`set_rate_range`** / **`rt_clk_set_rate`** 会调用并可能触发 **`clk_notify`** |
| `set_parent` / `set_phase` / `get_phase` / `round_rate` | 选父、相位、取整频率 |

若注册时 **`ops == NULL`**，**`rt_clk_register`** 会将其设为 **`unused_clk_ops`**（空结构），避免空指针。

### 2.4 `struct rt_clk_array` 与 `struct rt_clk_notifier`

- **`rt_clk_array`**：柔性数组成员 **`clks[]`**，配合 **`rt_ofw_get_clk_array`** 一次解析 **`clocks`** 多 phandle。
- **`rt_clk_notifier`**：挂入全局 **`_clk_notifier_nodes`**，在速率变化等时机调用 **`callback`**；消息位 **`RT_CLK_MSG_PRE_RATE_CHANGE`** 等定义在 **`clk.h`**。

---

## 3. 全局状态与锁（`clk.c`）

- **`_clk_lock`**：**`RT_DEFINE_SPINLOCK`**，保护节点链表、父子关系、notifier 链表、多数 **`rt_clk_*`** API 内部临界区。
- **`_clk_nodes`**：**无父** 的根 **`rt_clk_node`** 链表（如 **`fixed-clock`** 注册到全局树）。
- **`_clk_notifier_nodes`**：所有 notifier 的链表（遍历匹配 **`notifier->clk->clk_np`**）。

---

## 4. 节点注册与注销

### 4.1 `rt_clk_register(clk_np, parent_np)`

1. **`clk_np->clk` 先置 `NULL`**（注意后续分支）。
2. 若 **`ops` 为空**，赋 **`&unused_clk_ops`**。
3. **`rt_ref_init`**，初始化 **`list` / `children_nodes`**，**`multi_clk = 0`**。
4. **若有 `parent_np`**：**`clk_alloc(clk_np, NULL,NULL,NULL)`** 挂在 **`clk_np->clk`**，再 **`clk_set_parent`**：在锁内 **`clk_np->parent = parent_np`**，并把 **`clk_np->list`** 插入 **`parent_np->children_nodes`**。
5. **若无父节点**：**`parent = NULL`**，在锁内将 **`clk_np->list`** 插入 **`_clk_nodes`**（此时 **`clk_np->clk` 仍为 `NULL`**，与“纯提供者根节点”模型一致）。

### 4.2 `rt_clk_unregister(clk_np)`

在锁内要求：**无子节点** 且 **`rt_ref_read(&clk_np->ref) <= 1`**，才 **`rt_list_remove`** 并 **`clk_free(clk_np->clk)`**；否则 **`-RT_EBUSY`**。

---

## 5. 引用与创建 / 释放

- **`clk_get`**：**`rt_ref_get(&clk_np->ref)`**。
- **`clk_put`**：**`rt_ref_put(..., clk_release)`**；**`clk_release`** 当前 **`RT_ASSERT(0)`**，属占位实现。
- **`clk_create`**：**`clk_alloc` → `clk_get(clk_np)` → 可选 `ops->init`**；失败则 **`clk_free`**。
- **`clk_free`**：若 **`ops->finit`** 则调用，再 **`rt_free(clk)`**。
- **`rt_clk_put(clk)`**：**`clk_put(clk->clk_np)` + `clk_free(clk)`**，消费侧释放句柄的标准路径。

---

## 6. Prepare / Enable 与父子递归

逻辑模式与 Linux CCF 类似：**先父后子**（prepare）、**enable** 同理；**disable / unprepare** 顺序相反（代码中先递归子再处理当前，由 **`clk_disable`/`clk_unprepare`** 的递归顺序实现）。

- **`rt_clk_prepare`**：关中断语义下持锁调用 **`clk_prepare(clk, clk_np)`**；内部若存在 **`parent`**，先 **`clk_prepare(clk_np->clk, parent)`**（使用**子节点上绑定的 `clk_np->clk`** 作为沿树向上传递的句柄，与 OFW 动态创建的 **`struct rt_clk`** 可能不是同一指针，SoC 驱动需与注册方式一致）。
- **`prepare_count` / `enable_count`**：仅在从 0→1 时调用 **`ops->prepare`/`enable`**；反向在 1→0 时 **`unprepare`/`disable`**。

**`rt_clk_prepare_enable`**：先 **`prepare`**，**`enable`** 失败则 **`unprepare`**。

---

## 7. 频率与相位

- **`rt_clk_round_rate`**：若有 **`ops->round_rate`** 则调用并考虑 **`min_rate`/`max_rate` clamp**；否则在 min/max 间夹逼返回简单值。
- **`rt_clk_set_rate`**：先 **`rt_clk_round_rate`**；校验 min/max；调用 **`set_rate(clk, rate, parent_rate)`**，**`parent_rate`** 来自 **`rt_clk_get_rate(parent->clk)`**（父无 **`clk`** 句柄时传 **`NULL`**）。若 **`clk_np->rate`** 与旧值不同，调用 **`clk_notify(..., RT_CLK_MSG_PRE_RATE_CHANGE, old_rate, clk_np->rate)`**（注意：**当前源码未发 POST/ABORT 消息**）。
- **`rt_clk_set_rate_range` / `set_min_rate` / `set_max_rate`**：更新 **`clk_np`** 的 min/max，并在有 **`set_rate`** 时尝试 **`rt_clamp`** 当前 rate。
- **`rt_clk_get_rate`**：优先 **`clk->rate`**，否则 **`clk_np->rate`**。
- **`set_phase` / `get_phase`**：在锁内转 **`ops`**。

---

## 8. Notifier

- **`rt_clk_notifier_register`**：自增 **`clk->clk_np->notifier_count`**，**`list`** 插入 **`_clk_notifier_nodes`**。
- **`clk_notify`**：遍历链表，**`notifier->clk->clk_np == clk_np`** 则调用 **`callback`**；若返回 **`-RT_EIO`** 则中断遍历（视为硬件错误）。

**`rt_clk_notifier_unregister`**：按 **`clk_np` 匹配** 找到首个 notifier 并移除（若存在多 notifier 同节点需注意语义）。

---

## 9. OFW 集成（`#ifdef RT_USING_OFW`）

### 9.1 解析单路时钟

- **`ofw_get_clk_no_lock`**：解析 **`clocks` + `#clock-cells`** phandle，取 clock provider 的 OFW 节点；若 **`rt_ofw_data` 未就绪** 则 **`rt_platform_ofw_request`** 触发驱动 probe；再通过 **`rt_ofw_parse_object(..., RT_CLK_NODE_OBJ_NAME, "#clock-cells")`** 得到 **`rt_clk_node`**。
- 若 **`rt_ofw_count_of_clk > 1`**，按 **`args[0]`** 在 **`clk_np` 数组** 中下标选取多输出中的某一路。
- 最后 **`clk_create(clk_np, np->full_name, name, &clk_args, np)`**。

**`rt_ofw_get_clk` / `rt_ofw_get_clk_by_name`**：加锁包装。

### 9.2 `rt_ofw_get_clk_array`

统计 **`clocks`** phandle 个数，**`rt_calloc`** 分配 **`rt_clk_array` + clks[]**；在持锁循环中 **`ofw_get_clk_no_lock`**；失败则 **`rt_clk_array_put`** 回滚。

### 9.3 `rt_ofw_count_of_clk`

推断单个 clock provider 输出路数：优先 **`clk_np->multi_clk`**；否则解析 **`clock-indices`** 最大索引、或 **`clock-output-names`** 字符串个数、默认 **1**；结果写回 **`clk_np->multi_clk`** 缓存。

### 9.4 设备侧便捷 API

**`rt_clk_get_array` / `get_by_index` / `get_by_name`**：在 **`RT_USING_OFW`** 下转发到 **`rt_ofw_get_clk_*`**；否则无操作。

---

## 10. 时钟数组批量 API

**`rt_clk_array_prepare` / `enable` / `prepare_enable`**：对 **`clks[i]`** 顺序执行；中途失败则 **while 回滚** 已成功的项。

**`rt_clk_array_disable_unprepare`**：先 **`disable`** 再 **`unprepare`** 全部。

---

## 11. `clk-fixed-rate.c`（`fixed-clock`）

- **`INIT_SUBSYS_EXPORT(fixed_clk_drv_register)`** 注册 **`rt_platform_driver`**，**`compatible = "fixed-clock"`**。
- **`fixed_clk_probe`**：**`rt_calloc`** **`struct rt_clk_fixed_rate`**，**`fixed_clk_ofw_init`** 从节点读 **`clock-frequency`**（必选）、**`clock-accuracy`**、**`clock-output-names`**（可选覆盖 **`name`**），填写 **`clk_fixed->clk.rate/min_rate/max_rate`** 及 **`rt_ofw_data(np) = &clk_fixed->clk`**。
- **`rt_clk_register(&clk_fixed->clk, RT_NULL)`**：作为**根时钟节点**挂入 **`_clk_nodes`**，无 **`rt_clk_ops`** 时走 **`unused_clk_ops`**（固定频率无需 enable/set_rate 亦可工作于“只读 rate”场景）。

---

## 12. Kconfig / SConscript 小结

- **`RT_USING_CLK`**：`default y` 但依赖 DM，实际由根配置决定。
- **`osource "$(SOC_DM_CLK_DIR)/Kconfig"`**：SoC 可把专用时钟驱动 Kconfig 挂入。
- **SConscript**：始终编译 **`clk.c`**；**`RT_USING_OFW`** 时增加 **`clk-fixed-rate.c`**；若子目录存在 **`SConscript`** 也会并入（当前列表目录下无子文件夹脚本，预留扩展）。

---

## 13. BSP / SoC 扩展建议阅读顺序

1. **`clk.h`**：弄清 **`rt_clk_node` / `rt_clk` / `rt_clk_ops`** 与消息宏。  
2. **`clk.c`**：**`rt_clk_register` → prepare/enable → set_rate → OFW get**。  
3. **`clk-fixed-rate.c`**：设备树 **`fixed-clock`** 最小示例。  
4. 对照本 SoC 的 **`SOC_DM_CLK_DIR`** 中 PLL/MUX 驱动如何在 **`probe`** 里 **`rt_clk_register`** 并填充 **`ops`**。

---

## 14. 小结

| 项目 | 说明 |
|------|------|
| 目录职责 | DM 下的 **通用时钟框架**：节点树、消费句柄、prepare/enable 计数、改频、OFW 解析、notifier |
| 内建驱动 | **`fixed-clock`** → **`rt_clk_fixed_rate` + rt_clk_register** |
| 线程上下文 | **`prepare`/`unprepare`/`prepare_enable`/`disable_unprepare`** 标注不可在中断调用 |
| 注意点 | **`clk_release` 未完整实现**；**`rt_clk_set_rate` 仅发 PRE_RATE 类 notify**；notifier 注销按 **`clk_np` 匹配首项 |

按上述脉络阅读，可与 Linux **`clk_core.c` + `clk-fixed.c`** 类比，快速对齐 RT-Thread 5.2.0 的时钟抽象边界与扩展点。
