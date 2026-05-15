# RT-Thread 5.2.0 Sensor（传感器设备框架）代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/sensor` 目录实现的 **传感器设备框架**：在 **`RT_USING_SENSOR`** 下，通过 **`RT_USING_SENSOR_V2`** 在 **V1（`components/drivers/include/drivers/sensor.h` + `v1/`）** 与 **V2（`sensor_v2.h` + `v2/`）** 之间 **二选一** 编译与包含。核心为 **`struct rt_sensor_device`** 内嵌 **`struct rt_device parent`**，由 **`struct rt_sensor_ops`** 的 **`fetch_data`/`control`** 对接具体芯片驱动；框架负责 **设备注册名、互斥、中断脚、缓冲与 `rx_indicate` 通知语义**。

**`rtdevice.h`** 在 **`RT_USING_SENSOR`** 下按 **`RT_USING_SENSOR_V2`** 分支包含头文件（避免 **`sensor.h`** 与 **`sensor_v2.h`** 同时进入同一编译单元；二者均使用 **`#ifndef __SENSOR_H__`** 保护，工程侧应只选其一）：

```192:198:rt-thread-5.2.0/components/drivers/include/rtdevice.h
#ifdef RT_USING_SENSOR
#ifdef RT_USING_SENSOR_V2
#include "drivers/sensor_v2.h"
#else
#include "drivers/sensor.h"
#endif /* RT_USING_SENSOR_V2 */
#endif /* RT_USING_SENSOR */
```

---

## 1. 目录与编译

| 路径 | 作用 |
|------|------|
| **`v1/sensor.c`** | **V1** 设备接口：**`open`/`read`/`close`/`control`、`rt_hw_sensor_register`** |
| **`v1/sensor_cmd.c`** | **V1** MSH：**`sensor`/`sensor_polling`/`sensor_int`/`sensor_fifo`** |
| **`v2/sensor.c`** | **V2** 设备接口（**2022-12 重构**，与 V1 结构同源、API 不同） |
| **`v2/sensor_cmd.c`** | **V2** MSH（**`list`/`reset`/`probe`/`power` 等子命令**、类型/厂商/单位字符串） |
| **`Kconfig`** | 总开关、V2 开关、CMD 开关及 **V2+CMD 对 Klibc `vsnprintf` 的依赖** |
| **`SConscript`** | **V2 → 编 `v2/*`，否则 `v1/*`**；**`RT_USING_SENSOR_CMD`** 决定是否编 **`sensor_cmd.c`** |

**`Kconfig`** 要点：

```1:15:rt-thread-5.2.0/components/drivers/sensor/Kconfig
config RT_USING_SENSOR
    bool "Using Sensor device drivers"
    select RT_USING_PIN
    default n

if RT_USING_SENSOR
    config RT_USING_SENSOR_V2
        bool "Enable Sensor Framework v2"
        default n

    config RT_USING_SENSOR_CMD
        bool "Using Sensor cmd"
        select RT_KLIBC_USING_VSNPRINTF_STANDARD if RT_USING_SENSOR_V2
        default y
endif
```

**`SConscript`** 要点：

```8:15:rt-thread-5.2.0/components/drivers/sensor/SConscript
if GetDepend('RT_USING_SENSOR_V2'):
    src += [os.path.join('v2', 'sensor.c')]
    if GetDepend('RT_USING_SENSOR_CMD'):
        src += [os.path.join('v2', 'sensor_cmd.c')]
else:
    src += [os.path.join('v1', 'sensor.c')]
    if GetDepend('RT_USING_SENSOR_CMD'):
        src += [os.path.join('v1', 'sensor_cmd.c')]
```

---

## 2. V1 与 V2 差异总览

| 维度 | V1 | V2 |
|------|----|----|
| **传感器类型枚举宏** | **`RT_SENSOR_CLASS_*`** | **`RT_SENSOR_TYPE_*`**（及 **`*_STR` 展示字符串**） |
| **单条数据布局** | **`struct rt_sensor_data`**，多为 **`rt_int32_t` 等整型** | 同类结构体，物理量多为 **`rt_sensor_float_t`**（**`RT_USING_SENSOR_DOUBLE_FLOAT`** 可选 **`double`**） |
| **`rt_sensor_info`** | **`range_min/max`、`period_min` 等** | **`scale`、`accuracy`、`mode` 位域打包**（**采集方式 / 功耗 / 精度** 各占 4bit） |
| **`rt_sensor_config`** | 含 **`mode`、`power`、`odr`、`range`** 等 | 侧重 **`intf`、总线名、`irq_pin`**；**运行模式更多反映在 `info.mode` + control** |
| **标准 `control` 命令** | 见 **§3.1** | 见 **§3.2**（**与 V1 编号语义完全不同**） |
| **查找设备** | 应用自行 **`rt_device_find("前缀+名")`** | 额外提供 **`rt_sensor_device_find(const char *name)`**（**遍历类型前缀表** 再 **`rt_device_find`**） |
| **无脚中断** | **`RT_PIN_NONE`** | **`PIN_IRQ_PIN_NONE`**（与 **`dev_pin.h`** 一致） |

**关键结论**：**不可混用 V1/V2 头文件与驱动**；同一 **`RT_DEVICE_CTRL_BASE(Sensor)+n`** 在 V1/V2 上 **含义不同**（见下表）。

---

## 3. 标准 `control` 命令编号（务必区分版本）

### 3.1 V1（`sensor.h`）

| 宏 | 含义 |
|----|------|
| **`RT_SENSOR_CTRL_GET_ID`** | 读芯片 ID |
| **`RT_SENSOR_CTRL_GET_INFO`** | 框架在 **`rt_sensor_control`** 内 **`memcpy` 出 `sensor->info`**，**不**下发到 **`ops->control`** |
| **`RT_SENSOR_CTRL_SET_RANGE`** | 量程；成功则写 **`sensor->config.range`** |
| **`RT_SENSOR_CTRL_SET_ODR`** | 输出速率（Hz）；成功写 **`sensor->config.odr`** |
| **`RT_SENSOR_CTRL_SET_MODE`** | **POLLING / INT / FIFO**；与 **`open` 的 `oflag`** 配合 |
| **`RT_SENSOR_CTRL_SET_POWER`** | 功耗档；成功写 **`sensor->config.power`** |
| **`RT_SENSOR_CTRL_SELF_TEST`** | 自检 |
| **`> RT_SENSOR_CTRL_USER_CMD_START`（0x100）** | 透传到 **`ops->control`** |

### 3.2 V2（`sensor_v2.h`）

| 宏 | 含义 |
|----|------|
| **`RT_SENSOR_CTRL_GET_ID`** | 与 V1 同为 **Base+0**，仍下发到 **`ops->control`** |
| **`RT_SENSOR_CTRL_SELF_TEST`** | **Base+1**（**与 V1 的 `GET_INFO` 同偏移，语义完全不同**） |
| **`RT_SENSOR_CTRL_SOFT_RESET`** | **Base+2** |
| **`RT_SENSOR_CTRL_SET_FETCH_MODE`** | **Base+3**，参数为 **`RT_SENSOR_MODE_FETCH_*`**；成功则 **`RT_SENSOR_MODE_SET_FETCH(info.mode, …)`** |
| **`RT_SENSOR_CTRL_SET_POWER_MODE`** | **Base+4**，参数为 **`RT_SENSOR_MODE_POWER_*`** |
| **`RT_SENSOR_CTRL_SET_ACCURACY_MODE`** | **Base+5**，参数为 **`RT_SENSOR_MODE_ACCURACY_*`** |
| **`> RT_SENSOR_CTRL_USER_CMD_START`** | 透传到 **`ops->control`** |

**V2 框架层 `_sensor_control` 不再实现 `GET_INFO`**：应用应直接读 **`(rt_sensor_t)dev->info`**，或通过 **`sensor_cmd.c`** 的 **`list`** 等命令查看。  
**V2 注意**：若 **`SET_*_MODE` 参数非法**，当前实现存在 **`return -RT_EINVAL` 未释放 `module->lock`** 的路径，多传感器 module 场景下需驱动侧避免非法参数或自行规避（阅读 **`v2/sensor.c`** 中 **`_sensor_control`** 即可看到该早退）。

---

## 4. `sensor.c`：设备名前缀与中断回调

### 4.1 注册名 = 类型前缀 + 用户后缀

**V1** 前缀表（节选）与 **`rt_hw_sensor_register`** 拼接方式见：

```20:47:rt-thread-5.2.0/components/drivers/sensor/v1/sensor.c
static char *const sensor_name_str[] =
{
    "none",
    "acce_",     /* Accelerometer     */
    "gyro_",     /* Gyroscope         */
    "mag_",      /* Magnetometer      */
    ...
    "pow_"       /* Power sensor      */
};
```

**V2** 前缀更短，且 **仅覆盖到血压 `bp_` 为止**，表末为 **`RT_NULL`**，用于 **`rt_sensor_device_find`** 终止遍历：

```21:45:rt-thread-5.2.0/components/drivers/sensor/v2/sensor.c
static char *const sensor_name_str[] =
{
    "None",
    "ac-",       /* Accelerometer     */
    "gy-",       /* Gyroscope         */
    ...
    "bp-",       /* Blood Pressure    */
    RT_NULL
};
```

**`rt_hw_sensor_register(sensor, name, flag, data)`** 两版逻辑一致：**`calloc(strlen(前缀)+1+strlen(name))`** → **`memcpy` 前缀** → **`strcat` 后缀** → **`rt_device_register`** → **`rt_free(device_name)`**。  
因此 **`rt_device_find("acce_mpu")`（V1）** 与 **`rt_device_find("ac-mpu")`（V2）** 这类全名 **随版本变化**。

### 4.2 **`rt_sensor_cb` / `_sensor_cb` 与 `rx_indicate`**

中断服务里最终调用 **`rt_sensor_cb`（V1）** 或 **`_sensor_cb`（V2）**：先可选 **`irq_handle`**，再根据 **缓冲是否有数据** 或 **当前采集模式** 决定 **`rx_indicate(dev, size)`** 的 **`size`**（**有缓冲 → `data_len/sizeof(data)`；INT → 1；FIFO → `fifo_max`**）。  
**V1** 用 **`sensor->config.mode`**；**V2** 用 **`RT_SENSOR_MODE_GET_FETCH(sensor->info.mode)`**。

**Module**：**`irq_callback`/`_irq_callback`** 若 **`sensor->module` 非空**，则对 **`module->sen[0..sen_num-1]`** 逐个调用 **`rt_sensor_cb`/`_sensor_cb`**，实现 **一根 IRQ 线多颗逻辑传感器** 的联动通知。

### 4.3 **`rt_sensor_irq_init`**

根据 **`irq_pin.mode`**（**下拉输入 → 上升沿；上拉输入 → 下降沿；浮空输入 → 双边**）调用 **`rt_pin_attach_irq`** 并 **`rt_pin_irq_enable(..., RT_TRUE)`**。  
**`open`** 仅在 **INT/FIFO** 且底层 **`SET_MODE`（V1）或 `SET_FETCH_MODE`（V2）** 返回 **`RT_EOK`** 后才调用该初始化。

### 4.4 **`local_ops` 桩**

| 版本 | **`fetch_data` 未实现时** | **`control` 未实现时** |
|------|---------------------------|-------------------------|
| V1 | 返回 **0** | 返回 **`-RT_ERROR`** |
| V2 | 返回 **`-RT_EINVAL`** | 返回 **`-RT_EINVAL`** |

**`rt_hw_sensor_register`** 若 **`sensor->ops == RT_NULL`**，会赋 **`&local_ops`**，避免空指针；真实产品驱动应提供完整 **`ops`**。

---

## 5. `open` / `read` / `close` / `control` 行为摘要

### 5.1 **`open`**

- 若属于 **module** 且 **`fifo_max > 0`** 且尚无 **`data_buf`**：**`malloc` 长度为 `fifo_max` 的 `rt_sensor_data` 数组**。
- **`oflag` 与 `dev->flag` 交集**决定模式：
  - **`RT_DEVICE_FLAG_RDONLY`** → **轮询**
  - **`RT_DEVICE_FLAG_INT_RX`** → **中断**（成功则 **挂 IRQ**）
  - **`RT_DEVICE_FLAG_FIFO_RX`** → **FIFO**（成功则 **挂 IRQ**）
  - 否则 **`-RT_EINVAL`**
- **V1**：先 **`SET_MODE`**，再 **`SET_POWER` → `RT_SENSOR_POWER_NORMAL`**。  
- **V2**：**`SET_FETCH_MODE`**，再 **`SET_POWER_MODE` → `POWER_HIGHEST`**、**`SET_ACCURACY_MODE` → `ACCURACY_HIGHEST`**，并同步 **`info.mode` 位域**。

### 5.2 **`read`**

- **`module`** 存在时全程 **`mutex`**。
- **`data_len > 0`**：从 **`data_buf` 拷贝** 最多 **`data_len/sizeof(rt_sensor_data)`** 条，并 **清零 `data_len`**。
- 否则调用 **`ops->fetch_data`**。

### 5.3 **`close`**

- **`SET_POWER`/`SET_POWER_MODE` → DOWN**。
- **module 且 FIFO 缓冲已分配**：仅当 **module 内所有 sensor 的 `parent.ref_count` 均为 0** 时才 **逐颗 `rt_free(data_buf)`**（避免仍被打开引用时误释放）。
- **非轮询** 且 **IRQ 脚有效**：**`rt_pin_irq_enable(..., RT_FALSE)`**（**未** `detach`，与 V1/V2 一致）。

### 5.4 **`control`（设备 `rt_device_control` 入口）**

- **`module` 互斥**；若 **`ops->control` 非空** 则用它，否则用框架桩。
- **V1**：处理 **`GET_ID`、`GET_INFO`（仅框架）、SET_RANGE/ODR/POWER、SELF_TEST`**；其余 **`cmd > 0x100`** 透传。
- **V2**：处理 **`GET_ID`、`SET_ACCURACY_MODE`、`SET_POWER_MODE`、`SET_FETCH_MODE`、`SELF_TEST`、`SOFT_RESET`**；对 **标准 cmd** 在校验合法后调用 **`ops`** 并更新 **`info.mode`**；**`> 0x100`** 透传。

---

## 6. `sensor_cmd.c`（`RT_USING_SENSOR_CMD`）

- **V1**：**`sensor_show_data`** 按 **`RT_SENSOR_CLASS_*`** 分支；**FIFO 演示** 中 **`RT_SENSOR_CTRL_GET_INFO`** 取 **`fifo_max`** 再 **`malloc`**。
- **V2**：**`sensor_get_type_name`/`sensor_get_vendor_name`/`sensor_get_unit_name`/`sensor_get_power_mode_name`** 等与头文件宏一致；**`sensor … list`** 遍历 **`RT_Object_Class_Device`** 过滤 **`RT_Device_Class_Sensor`**，打印 **`info.mode` 三维与 `scale`/`accuracy`**；**`reset`** 调 **`RT_SENSOR_CTRL_SOFT_RESET`**；**`power`** 调 **`SET_POWER_MODE`**；**`probe`** 打开设备后 **`GET_ID`**。

两版均导出：**`sensor`**、**`sensor_polling`**、**`sensor_int`**、**`sensor_fifo`**，便于在 MSH 下验证三种 **`open` 标志** 与 **`rx_indicate` + 信号量** 模型。

---

## 7. 其它依赖

- **`RT_USING_SENSOR`** **`select RT_USING_PIN`**：中断路径依赖 **PIN 驱动**。
- **`rt_sensor_get_ts()`**（头文件内联或声明）：有 **`RT_USING_RTC`** 时倾向 **`time()`**，否则 **`rt_tick_get()`**，供 **`timestamp`** 字段。

---

## 8. 小结

| 组件 | 职责 |
|------|------|
| **`sensor.h` + `v1/sensor.c`** | 经典 **class/整型/config 字段** 模型与 **V1 control 集** |
| **`sensor_v2.h` + `v2/sensor.c`** | **type/float/mode 位域**、**扩展 control**、**`rt_sensor_device_find`** |
| **`sensor_cmd.c`** | MSH 自测、列表与读数打印 |

新 BSP 建议 **统一选用 V2** 并保持 **`RT_USING_SENSOR_CMD` + `RT_KLIBC_USING_VSNPRINTF_STANDARD`** 与 Kconfig 一致；维护旧板级驱动时可暂时保留 **V1**，迁移时重点替换 **设备全名前缀、`control` 编号与数据结构**。

---

*文档对应源码树版本：RT-Thread 5.2.0；分析根路径：`rt-thread-5.2.0/components/drivers/sensor/`。*
