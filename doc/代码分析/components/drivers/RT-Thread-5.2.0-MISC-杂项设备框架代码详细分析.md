# RT-Thread 5.2.0 MISC 杂项设备框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/misc` 目录：在 **`SConscript`** 中按 **Kconfig** 条件编译多类 **“杂项”外设框架**——**ADC、DAC、PWM、脉冲编码器、输入捕获**，以及 **NULL / ZERO / random / urandom** 伪设备。各子模块头文件位于 **`components/drivers/include/drivers/`**，由 **`rtdevice.h`** 在对应 **`RT_USING_*`** 宏下包含（**`null`/`zero`/`random`** 无专用头文件，实现自包含在 **`.c`** 中）。

---

## 1. 编译与文件对应

**`misc/Kconfig`** 与 **`misc/SConscript`** 对应关系：

| Kconfig | 源文件 |
|---------|--------|
| **`RT_USING_ADC`** | **`adc.c`** |
| **`RT_USING_DAC`** | **`dac.c`** |
| **`RT_USING_PWM`** | **`rt_drv_pwm.c`** |
| **`RT_USING_PULSE_ENCODER`** | **`pulse_encoder.c`** |
| **`RT_USING_INPUT_CAPTURE`** | **`rt_inputcapture.c`** |
| **`RT_USING_NULL`** | **`rt_null.c`** |
| **`RT_USING_ZERO`** | **`rt_zero.c`** |
| **`RT_USING_RANDOM`** | **`rt_random.c`**（内含 **`random` + `urandom`** 两套设备） |

**`SConscript`**：仅当 **`len(src) > 0`** 时加入 **`DeviceDrivers`** 组；**`CPPPATH = ../include`**。

**输入捕获环形缓冲**：**`RT_INPUT_CAPTURE_RB_SIZE`**（默认 100）在 **`Kconfig`** 中定义，经 **`rtconfig.h`** 供 **`rt_inputcapture.c`** 使用。

---

## 2. ADC（`adc.c` / `adc.h`）

### 2.1 设备模型

- **`struct rt_adc_device`** 继承 **`rt_device`**，**`RT_Device_Class_ADC`**
- **`struct rt_adc_ops`**：**`enabled`、`convert`（必选）**、**`get_resolution`、`get_vref`** 可选

### 2.2 框架行为

- **`read`**：**`pos` 为通道号**，循环 **`size` 次** 调用 **`ops->convert`**，每次向 **`buffer`** 写入一个 **`rt_uint32_t`**
- **`control`**：**`RT_ADC_CMD_ENABLE/DISABLE`**（走 **`ops->enabled`**）、**`RT_ADC_CMD_GET_RESOLUTION`**、**`RT_ADC_CMD_GET_VREF`**
- **`rt_hw_adc_register`**：断言 **`ops->convert`**，挂 **`read`/`control`**，**`RT_DEVICE_FLAG_RDWR`**

### 2.3 辅助 API

**`rt_adc_read` / `enable` / `disable`**；**`rt_adc_voltage`**：临时 **enable → convert → disable**，按 **分辨率与 Vref** 换算电压（**毫伏量级约定**，与 Finsh 打印一致）。

### 2.4 Finsh（**`RT_USING_FINSH`**）

**`MSH_CMD_EXPORT(adc, ...)`**：**`adc probe <name>`** 后 **enable/read/disable/voltage** 子命令。

**注意**：**`_adc_control`** 在 **未命中任何分支** 时 **`result` 可能未初始化** 即返回，调用方应只发已支持命令，或 BSP 应保证分支完备。

---

## 3. DAC（`dac.c` / `dac.h`）

### 3.1 设备模型

- **`RT_Device_Class_DAC`**
- **`struct rt_dac_ops`**：**`convert`（必选）**、**`enabled`/`disabled`、`get_resolution`**

### 3.2 框架行为

- **`write`**：以 **`sizeof(int)`** 为步进循环（**`i += sizeof(int)`**），**`ops->convert(dac, pos + i, value)`**——**`pos` 为起始通道/偏移语义**，与 ADC 的 **`pos` 作纯通道号** 用法不同，集成时需对照 BSP 约定。
- **`control`**：**`RT_DAC_CMD_ENABLE/DISABLE/GET_RESOLUTION`**
- **`rt_hw_dac_register`**：**`RT_DEVICE_FLAG_RDWR`**

---

## 4. PWM（`rt_drv_pwm.c` / `dev_pwm.h`）

### 4.1 设备模型

- **`struct rt_device_pwm`** + **`struct rt_pwm_ops`**（**`control`** 为主）

### 4.2 框架行为

- **`read`/`write`**：**`pos` 表示通道**（正/负与 **`configuration.channel`** 映射一致），缓冲区为 **`rt_uint32_t` pulse**；内部先 **`PWM_CMD_GET`** 再读/改 **`pulse`** 后 **`PWM_CMD_SET`**
- **`_pwm_control`**：对 **`PWMN_CMD_ENABLE/DISABLE`** 仅设置 **`configuration.complementary`** 标志，其余 **`cmd`** 转发 **`ops->control`**
- **`rt_device_pwm_register`**：**`rt_memset` 清零** 后注册，类型 **`RT_Device_Class_PWM`**

同文件后部还提供 **`rt_pwm_enable/disable`**、**周期/占空比设置**、**死区/相位** 等封装（与头文件 **`dev_pwm.h`** 中命令字一致，阅读时可继续向下翻 **`rt_drv_pwm.c`**）。

---

## 5. 脉冲编码器（`pulse_encoder.c` / `pulse_encoder.h`）

- 类型：**`RT_Device_Class_Miscellaneous`**
- **`init`** → **`ops->init`**
- **`open`/`close`** → **`ops->control(ENABLE/DISABLE)`**
- **`read`**：若 **`get_count`** 存在，将 **32 位计数** 写入 **`buffer`**，**固定返回 1**（表示“一个整型样本”，与 **`size`** 参数关系较弱，上层应按单 **`rt_int32_t` 读取** 理解）
- **`control`**：**`CLEAR_COUNT`、`GET_TYPE`、ENABLE/DISABLE** 透传
- **`rt_device_pulse_encoder_register`**：**`RT_DEVICE_FLAG_RDONLY | STANDALONE`**

---

## 6. 输入捕获（`rt_inputcapture.c` / `rt_inputcapture.h`）

- 类型：**`RT_Device_Class_Miscellaneous`**，**只读 + STANDALONE**
- **`init`**：默认 **`watermark = RB_SIZE/2`**，调 **`ops->init`**
- **`open`**：**`rt_ringbuffer_create(sizeof(rt_inputcapture_data) * RT_INPUT_CAPTURE_RB_SIZE)`**，再 **`ops->open`**
- **`close`**：先 **`ops->close`**，成功则 **`rt_ringbuffer_destroy`**
- **`read`**：从 **`ringbuff`** 取 **`struct rt_inputcapture_data`**，返回条数 **`receive_size / sizeof(data)`**
- **`control`**：**`CLEAR_BUF`、`SET_WATERMARK`**

### 6.1 `rt_hw_inputcapture_isr`

BSP 在捕获中断中调用：**`ops->get_pulsewidth`** 填 **`pulsewidth_us`**，组 **`data.is_high = level`**，**`rt_ringbuffer_put`**；若累计条数 **≥ watermark** 且注册了 **`rx_indicate`**，则通知上层。

---

## 7. NULL / ZERO / random（`rt_null.c`、`rt_zero.c`、`rt_random.c`）

三者均为 **静态 `struct rt_device`**，**`INIT_DEVICE_EXPORT`** 自动注册，**无 `rtdevice.h` 依赖**。

| 设备名 | read | write |
|--------|------|-------|
| **`null`** | 恒 **0** | 丢弃数据，返回 **`size`** |
| **`zero`** | **`memset(0)`** 填 **`buffer`**，返回 **`size`** | 丢弃，返回 **`size`** |
| **`random`** | **线性同余** **`214013*seed+2531011`**，取高位 **15 bit**，按 **`rt_uint16_t` 块填充**；残余字节 **`memcpy`** | 最多写入 **`sizeof(seed)`** 重设种子 |
| **`urandom`** | 与 **`random`** 相同算法但 **独立 `useed` 状态** | 写 **`useed`** |

**`control`** 均恒 **`RT_EOK`**。**`RT_ASSERT(!rt_device_find("..."))`**：若设备名已存在则 **断言失败**。

**安全说明**：**`random`/`urandom` 为弱伪随机**，不适合密码学场景。

---

## 8. `rtdevice.h` 包含关系（节选）

- **`RT_USING_ADC`** → **`drivers/adc.h`**
- **`RT_USING_DAC`** → **`drivers/dac.h`**
- **`RT_USING_PWM`** → **`drivers/dev_pwm.h`**
- **`RT_USING_PULSE_ENCODER`** → **`drivers/pulse_encoder.h`**
- **`RT_USING_INPUT_CAPTURE`** → **`drivers/rt_inputcapture.h`**

**`null`/`zero`/`random`**：无头文件导出，应用通过 **`rt_device_find("null")`** 等使用。

---

## 9. 小结

| 模块 | 设备类 | 主要数据路径 |
|------|--------|--------------|
| ADC | **ADC** | **`read(pos=ch)` → `convert`** |
| DAC | **DAC** | **`write` → `convert`** |
| PWM | **PWM** | **`read/write` pulse；`control` 扩展** |
| Pulse encoder | **Misc** | **`read` → `get_count`** |
| Input capture | **Misc** | **ISR → ringbuffer → `read`** |
| null/zero/random | **Misc** | **POSIX 风格伪设备** |

阅读顺序：按外设类型查阅对应 **`drivers/*.h`** → 回到 **`misc/*.c`** 中的 **`rt_device` 封装与 Finsh（若有）**。
