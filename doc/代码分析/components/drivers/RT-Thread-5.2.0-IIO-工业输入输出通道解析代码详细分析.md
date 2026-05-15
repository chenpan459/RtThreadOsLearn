# RT-Thread 5.2.0 IIO 目录代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/iio` 目录下的实现。与 Linux 内核中庞大的 **Industrial I/O（IIO）子系统**（`iio_device`、buffer、trigger、sysfs 等）不同，RT-Thread 5.2.0 在此处仅提供 **一层极薄的设备树（OFW）绑定辅助**：根据消费类设备节点上的 **`io-channels` / `io-channel-names`** 属性，解析出 **指向供应侧设备私有数据的 `void *`**，以及可选的 **通道编号参数**。具体 ADC/DAC 读写仍由各自驱动 API 完成。

涉及文件：

- 实现：`rt-thread-5.2.0/components/drivers/iio/iio.c`
- 头文件：`rt-thread-5.2.0/components/drivers/include/drivers/iio.h`
- 构建：`SConscript`（无独立 `Kconfig` 条目）
- 对外包含：`rtdevice.h` 在 **`#ifdef RT_USING_DM`** 块内 **`#include "drivers/iio.h"`**

---

## 1. 模块定位

```text
消费设备驱动（如触摸屏、电源管理芯片）
    ↓
rt_iio_channel_get_by_name / by_index(dev, ...)
    ↓
解析 dev->ofw_node 上 io-channels → phandle + #io-channel-cells
    ↓
rt_ofw_data(供应设备节点)  /* 通常为 ADC 等驱动在 probe 里挂的上下文指针 */
```

**结论**：本目录 **不** 实现采样缓冲、触发器、字符设备节点等完整 IIO 框架；命名 **IIO** 更接近 **Linux DT 中 “io-channel” 惯例** 在 RT-Thread DM 下的最小落地。

---

## 2. 编译与依赖

**`SConscript`**：

- 若 **未** 定义 **`RT_USING_DM`**，直接 **`Return('group')`**，**不编译** `iio.c`。
- 否则将 **`iio.c`** 加入 **`DeviceDrivers`** 组，**`CPPPATH`** 为 **`../include`**。

因此：**无 DM 则无 IIO 源文件**；头文件 **`iio.h`** 仅在 **`rtdevice.h`** 的 **`RT_USING_DM`** 分支中出现，与之一致。

---

## 3. API（`iio.h`）

| 函数 | 说明 |
|------|------|
| **`void *rt_iio_channel_get_by_index(struct rt_device *dev, int index, int *out_channel)`** | 按 **`io-channels`** 属性中第 **`index`** 组 phandle+参数解析 |
| **`void *rt_iio_channel_get_by_name(struct rt_device *dev, const char *name, int *out_channel)`** | 在 **`io-channel-names`** 中查找 **`name`** 得到索引，再转调 **`by_index`** |

**返回值**：供应侧节点上 **`rt_ofw_data(iio_np)`** 得到的 **`void *`**；失败为 **`RT_NULL`**。  
**`out_channel`**：非空时写入 **`#io-channel-cells`** 解析出的 **第一个 cell**（通常为硬件通道号或配置索引）。

---

## 4. 实现要点（`iio.c`）

### 4.1 `ofw_iio_channel_get_by_index`（静态，OFW 专用）

在 **`RT_USING_OFW`** 下：

1. **`rt_ofw_parse_phandle_cells(np, "io-channels", "#io-channel-cells", index, &iio_args)`**  
   从节点 **`np`** 解析第 **`index`** 组：**供应设备节点** 与 **cells 参数**。
2. **`iio_np = iio_args.data`**。
3. 若 **`rt_ofw_data(iio_np)`** 仍为 **`RT_NULL`**，调用 **`rt_platform_ofw_request(iio_np)`**，以 **推迟触发** 供应侧平台驱动的 probe（与 phandle 依赖顺序一致）。
4. **`iio = rt_ofw_data(iio_np)`**，**`rt_ofw_node_put(iio_np)`** 释放引用。
5. **`out_channel`** 存在时：**`*out_channel = iio_args.args[0]`**。

无 OFW 时该函数恒返回 **`RT_NULL`**。

### 4.2 `rt_iio_channel_get_by_index`

- **`dev == RT_NULL` 或 `index < 0`**：返回 **`RT_NULL`**。
- 若 **`dev->ofw_node`** 非空：调用 **`ofw_iio_channel_get_by_index`**；否则返回 **`RT_NULL`**。

### 4.3 `rt_iio_channel_get_by_name`

- **`dev`/`name` 为空**：返回 **`RT_NULL`**。
- **`index = rt_dm_dev_prop_index_of_string(dev, "io-channel-names", name)`**（定义见 **`dm.c`**，失败为负值或 **`-RT_ENOSYS`**）。
- 转调 **`rt_iio_channel_get_by_index(dev, index, out_channel)`**；非法 **`index`** 在 **`by_index`** 入口被拦截。

---

## 5. 设备树侧约定（与 Linux 对齐思路）

消费设备节点典型写法示例（逻辑说明，非强制 DTS 片段）：

- **`io-channels`**：若干 **`&adc 3`** 形式的 phandle + cell（cell 含义由供应驱动 **`#io-channel-cells`** 定义）。
- **`io-channel-names`**：与 **`io-channels`** 顺序一一对应的字符串，供 **`rt_iio_channel_get_by_name`** 查找。

供应侧（如 **`adc`** 控制器）驱动应在 **probe 完成** 后，对 **`pdev->parent.ofw_node`**（或等价节点）调用 **`rt_ofw_data_set(np, ctx)`**（或工程内实际使用的 OFW 私有数据挂载方式），使 **`rt_ofw_data(iio_np)`** 非空；否则 **`rt_platform_ofw_request`** 会尝试补 probe，但若驱动未正确注册仍可能得到 **`RT_NULL`**。

---

## 6. 使用注意

1. **仅 DM + OFW 场景有意义**；无 **`dev->ofw_node`** 时两 API 均得不到通道。
2. 返回的 **`void *`** 无统一结构体类型，**调用方需与具体供应驱动约定**（例如强转为某 **`struct xxx_adc *`**）。
3. **`out_channel`** 仅反映 DT 中 **第一个 cell**；若 **`#io-channel-cells > 1`**，其余参数需消费方自行扩展解析（当前 **`iio.c`** 未透出完整 **`iio_args`**）。
4. 仓库内 **`grep rt_iio_channel`** 命中范围很小，属 **基础设施层**；新 BSP 若不用 **`io-channels`**，可不关心本模块。

---

## 7. 小结

| 项目 | 说明 |
|------|------|
| 代码量 | **`iio.c`** 约 70 行，**无** 子目录与其它 `.c` |
| 职责 | **DT `io-channels` 解析 + 供应节点 `rt_ofw_data` 获取** |
| 依赖 | **`RT_USING_DM`**、**`RT_USING_OFW`**（实际解析路径） |
| 与 Linux IIO 关系 | **命名/DT 概念借鉴**；**非** Linux IIO 子系统移植 |

阅读顺序：**`iio.h`** → **`iio.c`** → 结合 **`rt_ofw_parse_phandle_cells`**、**`rt_platform_ofw_request`**（OFW 核心）理解 defer probe 行为。
