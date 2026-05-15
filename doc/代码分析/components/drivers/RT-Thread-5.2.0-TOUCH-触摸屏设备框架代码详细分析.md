# RT-Thread 5.2.0 Touch（触摸屏设备框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/touch` 目录实现的 **触摸屏设备框架**：在 **`RT_USING_TOUCH`** 下编译单文件 **`dev_touch.c`**，对外头文件为 **`components/drivers/include/drivers/dev_touch.h`**；**`rtdevice.h`** 在 **`RT_USING_TOUCH`** 下包含 **`drivers/dev_touch.h`**。

设计目标与 **Sensor 框架** 类似：为 **电容/电阻触摸 IC** 提供统一的 **`rt_device` 字符设备接口**（**`open`/`close`/`read`/`control` + `rx_indicate`**），**底层驱动**实现 **`struct rt_touch_ops`**（**读点、`touch_control`**），框架负责 **可选 GPIO 中断脚封装** 与 **`rt_hw_touch_isr` 通知路径**。

---

## 1. 目录与编译

| 文件 | 作用 |
|------|------|
| **`dev_touch.c`** | **`rt_hw_touch_register`、`rt_hw_touch_isr`、设备 `open/read/close/control`** |
| **`Kconfig`** | **`RT_USING_TOUCH`**；子项 **`RT_TOUCH_PIN_IRQ`**（**中断使用 PIN 子系统**） |
| **`SConscript`** | 依赖 **`RT_USING_TOUCH`** 与 **`RT_USING_DEVICE`**，编译 **`dev_touch.c`** |

**依赖**：头文件 **`#include "dev_pin.h"`**；若启用 **`RT_TOUCH_PIN_IRQ`**，需在 **menuconfig** 中同时保证 **PIN 驱动可用**（本目录 Kconfig **未** `select RT_USING_PIN`，由 BSP 配置保证）。

---

## 2. Kconfig

| 选项 | 含义 |
|------|------|
| **`RT_USING_TOUCH`** | 总开关（默认 **n**） |
| **`RT_TOUCH_PIN_IRQ`** | **`struct rt_touch_config` 中含 `irq_pin`**，框架用 **`rt_pin_attach_irq`** 接 **触摸 INT**；为 **n** 时 **开/关中断** 走 **`ops->touch_control(ENABLE/DISABLE_INT)`** |

---

## 3. 数据结构与操作表（`dev_touch.h`）

### 3.1 `struct rt_touch_info`

- **`type`**：**`RT_TOUCH_TYPE_*`**（电容 / 电阻等）。
- **`vendor`**：**`RT_TOUCH_VENDOR_*`**（如 GT / FT）。
- **`point_num`**：支持的最大触点数量（由驱动填充）。
- **`range_x` / `range_y`**：坐标范围；也可由 **`RT_TOUCH_CTRL_SET_X_RANGE` / `SET_Y_RANGE`** 在框架层同步 **`info`**。

### 3.2 `struct rt_touch_config`

- **`#ifdef RT_TOUCH_PIN_IRQ`**：**`irq_pin`**（**`struct rt_device_pin_mode`**），用于 **通知主机读取数据**。
- **`dev_name`**：常指 **I2C/SPI 等通信从设备名**（由具体驱动使用）。
- **`user_data`**：驱动私有。

### 3.3 `struct rt_touch_device`

- **`struct rt_device parent`**。
- **`info` / `config`**。
- **`const struct rt_touch_ops *ops`**。
- **`irq_handle`**：**驱动在触摸芯片中断逻辑里先执行的钩子**（可选），再由框架 **`rx_indicate`**。

### 3.4 `struct rt_touch_data`

单点：**`event`（NONE/UP/DOWN/MOVE）、`track_id`、`width`、`x_coordinate`、`y_coordinate`、`timestamp`**。  
**`rt_touch_get_ts()`**：有 **`RT_USING_RTC`** 用 **`time()`**，否则 **`rt_tick_get()`**，供驱动在填 **`timestamp`** 时与 **Sensor** 框架一致。

### 3.5 `struct rt_touch_ops`

| 成员 | 职责 |
|------|------|
| **`touch_readpoint`** | **`read` 入口**：从触摸 IC 取 **最多 `touch_num` 个 `rt_touch_data`**，返回 **实际点数** |
| **`touch_control`** | **实现各 `RT_TOUCH_CTRL_*`** 及驱动私有命令 |

---

## 4. `dev_touch.c` 行为说明

### 4.1 `rt_hw_touch_isr`

- 若 **`rx_indicate == NULL`** 直接返回。
- 若 **`irq_handle` 非空** 先调用（**驱动预处理**，如清中断标志、读 FIFO 准备）。
- 再 **`rx_indicate(&parent, 1)`**：**固定通知长度为 1**（与 **按点数 `read`** 分离：应用通常在回调里 **`read` 多点缓冲**）。

**不使用 PIN 封装时**：在 **SoC 触摸中断服务程序** 末尾 **手动调用 `rt_hw_touch_isr(touch)`**（头文件注释已说明）。

### 4.2 `RT_TOUCH_PIN_IRQ` 路径

- **`rt_touch_irq_init`**（在 **`open` 且 `oflag` 与 `dev->flag` 均含 `RT_DEVICE_FLAG_INT_RX`** 时）：按 **`irq_pin.mode`** 选择 **上升沿 / 下降沿 / 双边**，**`rt_pin_attach_irq` → `rt_pin_irq_enable(..., PIN_IRQ_ENABLE)`**。
- **`touch_irq_callback`** → **`rt_hw_touch_isr`**。
- **`RT_TOUCH_CTRL_ENABLE_INT` / `DISABLE_INT`**：**`rt_touch_irq_enable` / `rt_touch_irq_disable`**，内部为 **GPIO `rt_pin_irq_enable`**。

### 4.3 非 `RT_TOUCH_PIN_IRQ` 路径

- **使能/禁用中断** 仅调用 **`ops->touch_control(..., ENABLE_INT/DISABLE_INT)`**（由 **I2C 触摸控制器内部配置** 完成）。

### 4.4 `rt_touch_read`

- 校验 **`buf`/`len`** 后 **`return touch->ops->touch_readpoint(touch, buf, len)`**。

### 4.5 `rt_touch_control`

| `cmd` | 行为 |
|-------|------|
| **`RT_TOUCH_CTRL_SET_MODE`** | 下 **`ops->touch_control`**；若 **`args` 指向值为 `RT_DEVICE_FLAG_INT_RX`** 再 **`rt_touch_irq_enable`** |
| **`SET_X_RANGE` / `SET_Y_RANGE`** | 下 **`ops`**；成功则写 **`touch->info.range_x` 或 `range_y`** |
| **`DISABLE_INT` / `ENABLE_INT`** | **`rt_touch_irq_disable` / `enable`**（GPIO 或 **`touch_control`**） |
| **`GET_ID` / `GET_INFO` 及 default** | 透传 **`ops->touch_control`** |

**实现注意**：**`SET_Y_RANGE` 成功分支中 `LOG_D` 使用了 `range_x`**，应为 **`range_y`**，属笔误，不影响 **`info.range_y` 赋值**（赋值本身使用 **`args`** 正确）。

### 4.6 `rt_hw_touch_register`

- **`RT_Device_Class_Touch`**，**`RT_DEVICE_FLAG_STANDALONE`** 与调用方 **`flag`** 合并。
- 挂 **`open/close/read/control`**（或 **`rt_device_ops`**）。

---

## 5. 典型应用流程（与头文件示例一致）

1. **`rt_device_find`** → **`rt_device_open(..., RT_DEVICE_FLAG_INT_RX)`**。
2. **`rt_device_set_rx_indicate`**：在回调里 **`control(DISABLE_INT)`**（避免重入）、**`read` 多点数据`**，再 **`control(ENABLE_INT)`**。
3. **`read` 缓冲** 为 **`struct rt_touch_data` 数组**，长度 **≤ `info.point_num`**（由 **`GET_INFO`** 或驱动文档确定）。

---

## 6. 与 BSP 驱动的分工

| 层次 | 职责 |
|------|------|
| **`dev_touch.c`** | **设备注册、GPIO IRQ 可选封装、`rx_indicate` 触发** |
| **芯片驱动（通常在 `bsp`/`libraries`）** | 填充 **`rt_touch_ops`**、维护 **`info`**、实现 **`touch_readpoint`/`touch_control`**、可选设置 **`irq_handle`** |

---

## 7. 小结

本目录为 **轻量级触摸抽象层**，代码量小、与 **PIN + `rt_device` 中断读模型** 紧耦合；**多点轨迹、手势、与 GUI 绑定** 一般在 **上层软件** 完成。若项目使用 **设备树 + 专用触摸子系统**，可能另有 **Input/DRM** 路径，本框架仍适用于 **裸 `read` 轮询/中断** 场景。

---

*文档对应源码树版本：RT-Thread 5.2.0；根路径：`rt-thread-5.2.0/components/drivers/touch/`。*
