# RT-Thread 5.2.0 PIN（通用 GPIO）设备框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/pin` 目录实现的 **通用 Pin/GPIO 设备层**：提供 **`rt_pin_*`** 应用 API、单实例 **`rt_device_pin_register`** 传统路径，以及在 **`RT_USING_DM`** 下的 **多 GPIO 控制器聚合**、**逻辑引脚号全局编址**、与 **`RT_USING_PIC`** 结合的 **GPIO 子中断级联**，以及 **`RT_USING_OFW`** 下的 **`gpios`/`xxx-gpios` 属性解析**。对外接口集中在 **`components/drivers/include/drivers/dev_pin.h`**；**`rtdevice.h`** 在 **`RT_USING_PIN`** 下包含 **`drivers/dev_pin.h`**。

---

## 1. 目录与编译开关

| 文件 | 条件 | 作用 |
|------|------|------|
| **`dev_pin.c`** | **`RT_USING_PIN`** | 单例 **`_hw_pin`**、**`rt_device_pin_register`**、**`rt_pin_*`** 转发、**`RT_USING_FINSH`** 下 **`pin` MSH 命令** |
| **`dev_pin_dm.c`** | **`RT_USING_DM`** | **`pin_api_*`** 聚合层、**`pin_api_init`**、**`pin_pic_*`**（**`struct rt_pin_irqchip`** + **`rt_pic` 级联**） |
| **`dev_pin_ofw.c`** | **`RT_USING_OFW`**（且 DM 打开时由上层使用） | **`rt_ofw_get_named_pin`** / **`rt_ofw_get_named_pin_count`** |
| **`dev_pin_dm.h`** | 仅 **`dev_pin_dm.c`** 包含 | **`pin_api_init`**、**`pin_pic_init`**、**`pin_pic_handle_isr`** 声明 |

**`pin/Kconfig`**：`menuconfig RT_USING_PIN`（默认 **y**），并 **`osource "$(SOC_DM_PIN_DIR)/Kconfig"`** 允许 SoC/BSP 追加 **DM Pin** 片段。

**`pin/SConscript`**：无 **`RT_USING_PIN`** 则不编译；**`RT_USING_DM`** 增加 **`dev_pin_dm.c`**；**`RT_USING_OFW`** 增加 **`dev_pin_ofw.c`**。

---

## 2. 数据结构与操作表（`dev_pin.h`）

### 2.1 `struct rt_device_pin`

- 继承 **`struct rt_device parent`**。
- **`const struct rt_pin_ops *ops`**：由具体 GPIO 驱动实现。
- **`#ifdef RT_USING_DM`**（**须保持 `parent` 之后成员顺序**，注释已说明）：
  - **`struct rt_pin_irqchip irqchip`**：内嵌 **`struct rt_pic parent`**，使 GPIO 控制器可作为 **PIC 子节点** 参与中断路由。
  - **`pin_start` / `pin_nr`**：本设备在 **全局逻辑引脚空间** 中的起始下标与数量（由 **`pin_api_init`** 分配）。
  - **`list`**：挂入 **`pin_nodes`** 链表。
  - **`legacy_isr`**：指向 **`rt_pin_irq_hdr` 数组**，在驱动未实现 **`pin_attach_irq`** 时由 DM 层缓存 **“旧式”回调**。

### 2.2 `struct rt_pin_irqchip`

- **`struct rt_pic parent`** + **`int irq`**（**父中断控制器**上用于 **级联 GPIO 汇总中断** 的逻辑 **IRQ 号**）+ **`pin_range[2]`**（头文件中保留字段，当前 **`pin/`** 实现主要使用 **`pin_start`/`pin_nr`** 表达范围）。

### 2.3 `struct rt_pin_ops`

| 成员 | 含义 |
|------|------|
| **`pin_mode` / `pin_write` / `pin_read`** | 方向/模式与电平读写（**DM 下 `pin` 为相对本设备的偏移**）。 |
| **`pin_attach_irq` / `pin_detach_irq` / `pin_irq_enable`** | 可选；若 **`pin_attach_irq` 为空**，DM 走 **`pin_irq_mode` + `legacy_isr`** 路径。 |
| **`pin_get`** | 字符串名（如 **`PA.16`**）→ 编号；**DM 下** 遍历各 **`rt_device_pin`** 直至某 **`ops->pin_get`** 返回成功。 |
| **`pin_debounce`** | 去抖时间配置。 |
| **`pin_irq_mode` / `pin_parse`** | 仅 **`RT_USING_DM`**：前者配置边沿/电平；后者解析 **`#gpio-cells`** 与 **DT flags**（**`dt-bindings/pin/pin.h`**）。 |
| **`pin_ctrl_confs_apply`** | **`RT_USING_PINCTRL`** 下由 **pinctrl** 子系统使用，本目录不实现。 |

### 2.4 常量与辅助结构

- **`PIN_LOW` / `PIN_HIGH`**、**`PIN_MODE_*`**、**`PIN_IRQ_MODE_*`**、**`PIN_IRQ_ENABLE/DISABLE`**。
- **`PIN_NONE`**（**`-RT_EEMPTY`**）：表示无效引脚或未配置。
- **`struct rt_device_pin_mode` / `rt_device_pin_value`**：配合 **`dev_pin.c`** 中 **`read`/`write`/`control`** 设备接口。
- **`struct rt_pin_irq_hdr`**：**单引脚** 的 **mode + 回调 + args**。

---

## 3. `dev_pin.c`：传统单实例模型

### 3.1 设备注册

- 静态全局 **`struct rt_device_pin _hw_pin`**。
- **`rt_device_pin_register(name, ops, user_data)`**：初始化 **`RT_Device_Class_Pin`**，挂接 **`rt_device_ops`** 或手写 **`read/write/control`**，调用 **`rt_device_register`**。

### 3.2 字符设备语义

- **`_pin_read`/`_pin_write`**：缓冲区为 **`struct rt_device_pin_value`**，内部调 **`ops->pin_read`/`pin_write`**。
- **`_pin_control`**：参数为 **`struct rt_device_pin_mode`**，调 **`ops->pin_mode`**。

### 3.3 **`rt_pin_*` API**

全部断言 **`_hw_pin.ops`** 非空，并转发到 **`_hw_pin.ops`**；**`rt_pin_attach_irq`/`detach`/`irq_enable`/`debounce`** 在对应 **ops 指针为空** 时返回 **`-RT_ENOSYS`**。

**要点**：此路径下 **全局仅一个** **`rt_device_pin`**；多组 GPIO 需走 **DM 聚合**（见下一节）。

### 3.4 MSH **`pin` 命令**（**`RT_USING_FINSH`**）

子命令 **`num` / `mode` / `read` / `write`**：支持 **符号名**（经 **`rt_pin_get`**）或 **整型引脚号**；模式字符串与 **`PIN_MODE_*`** 对应。

---

## 4. `dev_pin_dm.c`：设备模型下的聚合与 PIC

### 4.1 全局逻辑引脚空间

- **`pin_total_nr`**：下一个可用的 **全局起始下标**。
- **`pin_nodes`**：**已注册** 的 **`struct rt_device_pin`** 链表。
- **`pin_lock`**：**`pin_device_find`**、**`pin_api_init`**、**`pin_api_get`** 使用的自旋锁。

**`pin_device_find(rt_ubase_t pin)`**：在链表中查找满足 **`pin_start <= pin < pin_start + pin_nr`** 的 **`gpio`**，用于将 **全局引脚号** 路由到具体设备。

### 4.2 **`pin_api_*` 适配层**

注册名固定为 **`"gpio"`** 的 **`pin_api_dm_ops`**，在首次 **`pin_api_init`** 时若链表为空则 **`rt_device_pin_register("gpio", &pin_api_dm_ops, NULL)`**，从而 **复用 `dev_pin.c` 的 `_hw_pin` 与 `rt_pin_*` 入口**，但实际硬件操作分发给 **匹配到的 `gpio`**：

- **`pin_api_mode`/`write`/`read`**：将 **`pin` 减去 `gpio->pin_start`** 后调用 **`gpio->ops`**。
- **`pin_api_attach_irq`**：
  - 若 **`gpio->ops->pin_attach_irq` 存在**：直接转发（**相对引脚下标**）。
  - 若不存在：**`pin_irq_mode`** 配置硬件后，写入 **`legacy_isr[pin_index]`**（**`hdr`/`args`/`mode`**），兼容 **仅实现电平/边沿与使能、不实现 attach 的老驱动**。
- **`pin_api_detach_irq`**：无 **`pin_detach_irq`** 时 **`rt_memset` 清空 `legacy_isr` 项**。
- **`pin_api_irq_enable`**：要求 **`ops->pin_irq_enable`** 存在，否则 **`-RT_EINVAL`**（与 **legacy** 路径组合使用时需注意驱动是否实现使能接口）。
- **`pin_api_get`**：遍历各 **`gpio->ops->pin_get`**，**`!(res = gpio->ops->pin_get(name))`** 时认为成功——即 **返回 0** 表示解析到编号（与 **`rt_pin_get`** 文档中“返回引脚号”的常见用法一致，**0 号引脚** 需驱动约定避免歧义）。

### 4.3 **`pin_api_init(gpio, pin_nr)`**

- 校验 **`gpio`** 与 **`ops`**。
- 在锁内：若 **`pin_nodes`** 为空则先 **`rt_device_pin_register("gpio", ...)`**。
- **`gpio->pin_start = pin_total_nr`**，**`gpio->pin_nr = pin_nr`**，**`pin_total_nr += pin_nr`**，将 **`gpio`** 插入 **`pin_nodes`**。

**调用约定**：BSP/驱动在 **`rt_device_register`** 之前或之后按平台顺序调用 **`pin_api_init`**，保证 **各控制器 `pin_start` 不重叠**。

### 4.4 **`struct rt_pic_ops pin_dm_ops`（GPIO 作为子 PIC）**

为把 **“某根 GPIO 线”** 映射为 **PIC 体系中的逻辑 IRQ**，内嵌 **`rt_pin_irqchip.parent`** 充当 **`struct rt_pic`**：

| `rt_pic_ops` 成员 | 实现函数 | 行为摘要 |
|-------------------|----------|----------|
| **`irq_mask` / `irq_unmask`** | **`pin_dm_irq_mask` / `pin_dm_irq_unmask`** | 调底层 **`pin_irq_enable(..., 0/1)`**，与 **`PIN_IRQ_DISABLE`/`PIN_IRQ_ENABLE`** 一致。 |
| **`irq_enable` / `irq_disable`** | 同上交叉绑定 **`mask`/`unmask`** | 与 **`irq_mask`/`irq_unmask`** 相同函数指针组合，**从命名上看与常见 “enable=开中断” 直觉不一致**；实际以 **`pin_irq_enable` 第三参数 0/1** 为准。若上层仅使用 **`rt_pic_irq_mask`/`unmask`**，行为与 **`mask`/`unmask`** 列一致。 |
| **`irq_set_triger_mode`** | **`pin_dm_irq_set_triger_mode`** | **`RT_IRQ_MODE_*` ↔ `PIN_IRQ_MODE_*`** 映射。 |
| **`irq_map`** | **`pin_dm_irq_map`** | **`rt_pic_config_irq`** 后 **`rt_pic_cascade(pirq, gpio->irqchip.irq)`**，把 **每根线的 `rt_pic_irq` 挂到父 PIC 的汇总 IRQ**；再 **`rt_pic_irq_set_triger_mode`**。 |
| **`irq_parse`** | **`pin_dm_irq_parse`** | 期望 **2 个 cell**：**`hwirq`**（此处为 **引脚下标**）、**`mode`**（**`RT_IRQ_MODE_*` 掩码**）。 |

**未实现**：**`irq_ack`/`irq_eoi`/MSI** 等；GPIO 线级 **ACK/EOI** 由 **父中断** 在 SoC 侧统一处理，子 PIC 只负责 **mask/unmask/触发方式**。

### 4.5 **`pin_pic_init(gpio, pin_irq)`**

- 设置 **`irqchip.irq = pin_irq`**（**父级逻辑 IRQ**）。
- **`rt_calloc` 分配 `legacy_isr`**，失败返回 **`-RTENOMEM`**。
- **`pic->priv_data = gpio`**，**`pic->ops = &pin_dm_ops`**，将 **`gpio->parent.parent.type`** 置为 **`RT_Object_Class_Device`** 以满足 **`rt_pic_dynamic_cast`** 对嵌入 **`rt_pic`** 的布局识别（参见 **PIC 文档**）。
- **`rt_pic_linear_irq(pic, gpio->pin_nr)`** 为每根引脚申请 **`rt_pic_irq` 槽位**；**`rt_pic_user_extends(pic)`** 供平台弱符号扩展。

### 4.6 **`pin_pic_handle_isr(gpio, pin)`**

在 **父中断服务程序** 中根据 **硬件报告的引脚下标** `pin`（**相对本 `gpio` 设备**）调用：

1. **`rt_pic_find_irq(&irqchip->parent, pin_index)`** → 若 **`pirq->irq >= 0`**，**`rt_pic_handle_isr(pirq)`** 走 **PIC 链上的 ISR**（含级联语义）。
2. 无论是否走 PIC，若 **`legacy_isr[pin_index].hdr`** 非空，再 **直接调用 legacy 回调**。

因此 **同一引脚** 可同时存在 **PIC 挂载的 ISR** 与 **legacy 回调**（迁移期或特殊驱动需注意重复执行风险）。

---

## 5. `dev_pin_ofw.c`：设备树命名引脚

### 5.1 属性命名规则

对属性名 **`propname`** 依次尝试：

- **`"{propname}-gpios"`**、**`"{propname}-gpio"`**（`propname` 非空时）
- **`"gpios"`**、**`"gpio"`**（`propname` 为空时）

与 Linux 设备树习惯 **`xxx-gpios`** 对齐。

### 5.2 **`rt_ofw_get_named_pin`**

1. **`rt_ofw_parse_phandle_cells`**：解析 **`#gpio-cells`**，得到 **`pin_args`**（含 **GPIO 控制器节点 `pin_dev_np`**）。
2. 若 **`rt_ofw_data(pin_dev_np)`** 为空，**`rt_platform_ofw_request(pin_dev_np)`** 触发 **延迟 probe**，再取 **`pin_dev`**。
3. 若驱动实现 **`pin_parse`**：由其解析 **cells** 与 **`flags`**，并可把 **`dt-bindings/pin/pin.h`** 中的 **开漏/上下拉/有效电平** 转为 **`PIN_MODE_*`/`PIN_LOW`/`PIN_HIGH`** 写入 **`out_mode`/`out_value`**。
4. 否则默认 **`args[0]`** 为 **本地引脚号**。
5. 成功时 **返回值 `pin += pin_dev->pin_start`**，得到 **`pin_api` 全局逻辑编号**。

**参数校验**：源码为 **`if (!np && index < 0)`** 返回 **`EINVAL`**；与 **`rt_ofw_parse_phandle_cells`** 常规前置条件（**`np` 非空、`index>=0`**）相比偏窄，调用方应保证 **`np` 有效**。

### 5.3 **`rt_ofw_get_named_pin_count`**

循环构造与 **`get_named_pin`** 相同的 **`gpios_name`**，但当前实现里 **`rt_ofw_count_phandle_cells(np, propname, "#gpio-cells")`** 使用 **`propname` 原始串**而非 **`gpios_name`**，与 **`get_named_pin`** 的探测逻辑**不完全对称**；若 **`propname`** 仅为前缀（依赖 **`-gpios` 后缀**），计数可能与解析结果不一致——**以实际 DT 属性名为准做验证**。

### 5.4 与 **`dev_pin.h`** 的衔接

**`rt_pin_get_named_pin` / `rt_pin_get_named_pin_count`** 在 **`dev_pin.h`** 中声明、**`dev_pin_dm.c`** 实现：若 **`dev->ofw_node`** 为空则 **`-RT_EINVAL`**，否则转调 **`rt_ofw_*`**（**无 OFW 编译时** 返回 **`-RT_ENOSYS`**）。

---

## 6. 初始化与使用关系小结

```text
[无 DM]  rt_device_pin_register("gpio", board_ops, ...)
              -> rt_pin_* 直接调用 board_ops（单实例 _hw_pin）

[有 DM]  各 rt_device_pin：pin_api_init(gpio, pin_nr) 分配全局 pin_start
              -> 首次 pin_api_init 注册聚合层 "gpio" + pin_api_dm_ops
              -> rt_pin_* -> pin_device_find -> 各驱动 ops（相对引脚）

[DM + PIC] pin_pic_init(gpio, parent_irq)
              -> rt_pic_linear_irq + rt_pic_cascade 到 parent_irq
              -> 父 ISR 中 pin_pic_handle_isr(gpio, hw_pin_index)
```

---

## 7. 与周边子系统依赖

| 依赖 | 说明 |
|------|------|
| **`RT_USING_DM`** | **`struct rt_device`** 扩展字段、**`pin_api_*`**、**`rt_pin_get_named_pin`**、**`rt_pin_irqchip`**。 |
| **`RT_USING_OFW`** | **`dev_pin_ofw.c`**、**`rt_ofw_parse_phandle_cells`**、**`rt_platform_ofw_request`**。 |
| **`RT_USING_PIC`** | **`dev_pin.h`** 包含 **`pic.h`**；**`pin_pic_*`** 依赖 **`rt_pic_*`** API。 |
| **`dt-bindings/pin/pin.h`** | **`dev_pin_ofw.c`** 中 **flags** 解析（**`PIN_OPEN_DRAIN`**、**`PIN_PULL_*`**、**`PIN_ACTIVE_*`**）。 |

---

## 8. 小结

| 层次 | 职责 |
|------|------|
| **`dev_pin.h`** | **API/ops/设备结构** 与 **DM/PIC/Pinctrl/OFW** 条件编译边界 |
| **`dev_pin.c`** | **单实例 + Finsh** |
| **`dev_pin_dm.c`** | **多实例聚合、全局编号、`rt_pic` 子中断、legacy ISR** |
| **`dev_pin_ofw.c`** | **DT `gpios` 解析 → 全局引脚号 + 模式/初值** |

阅读 BSP 时建议：**先确认是否 `pin_api_init`**，再查 **`pin_pic_init` 与父 IRQ 的衔接**，最后在驱动 **`probe`/`attach`** 中用 **`rt_pin_get_named_pin`** 取 **LED/复位 GPIO** 等。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/pin/`。*
