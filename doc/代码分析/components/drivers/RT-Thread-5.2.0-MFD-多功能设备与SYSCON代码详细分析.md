# RT-Thread 5.2.0 MFD 多功能设备目录代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/mfd` 目录。在 RT-Thread 5.2.0 中，该目录**仅实现一类 MFD 相关能力**：**Syscon（System Controller）**——对设备树中 **`compatible = "syscon"`** 节点的 **`reg`** 区域做 **`ioremap`**，并提供 **32 位读/写/按位更新** 与 **按 OFW 节点查找** 的 API。头文件为 **`components/drivers/include/drivers/syscon.h`**，`rtdevice.h` 在 **`RT_USING_DM`** 且 **`RT_MFD_SYSCON`** 下包含 **`drivers/syscon.h`**。

涉及文件：

- **`mfd-syscon.c`**：Syscon 探测、链表、**`rt_syscon_*` API**
- **`Kconfig`、`SConscript`**

Linux 内核中的 **MFD（Multi-Function Device）** 子系统还包含大量 PMIC/codec 等复合驱动；本目录**未**移植其全貌，**仅 Syscon 一条线**。

---

## 1. Kconfig 与编译

| 选项 | 含义 |
|------|------|
| **`RT_USING_MFD`** | MFD 组总开关；**`depends on RT_USING_DM`**；**`SConscript`** 无此选项则不编本目录 |
| **`RT_MFD_SYSCON`** | 编译 **`mfd-syscon.c`**；**`depends on RT_USING_MFD`、`RT_USING_OFW`**；默认 **y**（在 **`RT_USING_MFD`** 打开时） |

**`SConscript`**：**`CPPPATH = ../include`**；**`RT_MFD_SYSCON`** 时 **`src += ['mfd-syscon.c']`**。

**与头文件关系**：源码依赖 **`RT_USING_MFD`**；**`syscon.h`** 的包含条件为 **`RT_MFD_SYSCON`**（在 DM 分支内）。实际产品需 **同时** 打开 **`RT_USING_MFD`** 与 **`RT_MFD_SYSCON`** 才能既有实现又有声明。

---

## 2. 数据结构（`syscon.h`）

**`struct rt_syscon`**：

| 成员 | 含义 |
|------|------|
| **`list`** | 挂入全局 **`_syscon_nodes`** |
| **`np`** | 对应 **OFW 节点指针** |
| **`iomem_base`** | **`rt_ioremap`** 后的虚拟基址 |
| **`iomem_size`** | **`reg` 区域长度**（字节） |
| **`rw_lock`** | 保护 **32 位 MMIO** 访问 |

---

## 3. MMIO 访问 API（`mfd-syscon.c`）

### 3.1 `rt_syscon_read` / `rt_syscon_write`

- 校验 **`offset < iomem_size`**（**字节偏移**；访问单位为 **32 位**，调用方应保证 **`offset` 四字节对齐**，否则行为依赖硬件/总线）。
- **`HWREG32(iomem_base + offset)`** 读写；外围由 **`syscon->rw_lock`** 关中断自旋锁保护。

### 3.2 `rt_syscon_update_bits`

在锁内：**`old = HWREG32(...)`**，**`old &= ~mask`**，写回 **`old | val`**。典型用于 **掩码字段** 修改。

---

## 4. 查找与延迟探测

### 4.1 全局链表

- **`_syscon_nodes`** + **`_syscon_nodes_lock`**
- **`syscon_probe`** 成功后将 **`syscon->list`** 插入链表

### 4.2 `rt_syscon_find_by_ofw_node`

1. 持锁在 **`_syscon_nodes`** 中按 **`syscon_tmp->np == np`** 查找已存在实例。
2. 若未找到且节点 **compatible** 为 **`"syscon"`** 或 **`"simple-mfd"`**，构造**栈上** **`struct rt_platform_device`**，仅 **`parent.ofw_node = np`**，调用 **`syscon_probe(&syscon_pdev)`** 尝试现场探测。
3. 成功则 **`rt_ofw_data(np)`** 已被设为 **`struct rt_syscon *`**（见 **`syscon_probe`**），返回该指针。

**说明**：平台驱动 **`syscon_ofw_ids`** 仅注册 **`"syscon"`**；**`"simple-mfd"`** 节点通常由子设备或 **`find_by_ofw_node`** 的 **lazy probe** 路径创建 **syscon 句柄**（需节点带合法 **`reg`**，与 Linux **syscon** 用法一致思路）。

### 4.3 `rt_syscon_find_by_ofw_compatible`

**`rt_ofw_find_node_by_compatible`** → **`rt_syscon_find_by_ofw_node`**，并 **`rt_ofw_node_put`**。

### 4.4 `rt_syscon_find_by_ofw_phandle`

**`rt_ofw_parse_phandle(np, propname, 0)`** 得 **syscon 节点** → **`rt_syscon_find_by_ofw_node`**。

---

## 5. 平台驱动 `syscon_driver`

### 5.1 `syscon_probe`

1. **`rt_calloc`** **`struct rt_syscon`**
2. **`rt_ofw_get_address(np, 0, &addr, &size)`** 取 **`reg`**
3. **`rt_ioremap(addr, size)`**
4. 插入 **`_syscon_nodes`**，**`rt_spin_lock_init(&rw_lock)`**
5. **`pdev->parent.user_data = syscon`**（注意此处 **`pdev` 为真实 platform 设备**）
6. **`syscon->np = np`**，**`rt_ofw_data(np) = syscon`**

返回 **`RT_EOK`**；失败 **`rt_free(syscon)`**（**未** 对已成功 **`ioremap`** 的路径单独 **`iounmap`**——当前失败点仅在 **`ioremap` 失败** 之前）。

### 5.2 `syscon_remove`

**`rt_iounmap`**、**`rt_free`**；**未** 从 **`_syscon_nodes`** 摘除，**未** 清除 **`rt_ofw_data(np)`** 与 **`user_data`**。若内核支持动态卸载该驱动，存在**链表悬挂/双次释放风险**；静态常驻场景影响较小。属实现完整性问题，文档如实记录。

### 5.3 注册方式

**`INIT_SUBSYS_EXPORT(syscon_drv_register)`** 内 **`rt_platform_driver_register`**，**非** **`RT_PLATFORM_DRIVER_EXPORT`** 宏形式。

---

## 6. 设备树与使用建议

- 典型节点：**`compatible = "syscon"`**，**`reg = <基址 长度>`**；其它驱动通过 **`syscon = <&...>`** 属性 **`rt_syscon_find_by_ofw_phandle(dev->ofw_node, "syscon")`** 获取句柄。
- **偏移**：与 SoC 手册一致，注意 **大小端**（当前实现固定 **32 位 little-endian 风格 `HWREG32`**，与 ARM 常见 MMIO 一致）。
- **多核/电源域**：Syscon 仅为 **寄存器访问封装**，不涉及时钟/复位联动；后者应配合 **`clk`/`reset`** 等子系统。

---

## 7. 小结

| 项目 | 说明 |
|------|------|
| 目录体量 | **单文件 `mfd-syscon.c`** + **`syscon.h`** |
| 功能 | **Syscon MMIO：read/write/update_bits + OFW 查找/lazy probe** |
| 依赖 | **DM、OFW、IOREMAP** |
| 与 Linux MFD | **概念对齐在 syscon；无通用 MFD cell 框架** |

阅读顺序：**`syscon.h`** → **`mfd-syscon.c`**（**`find_by_ofw_node` 与 `syscon_probe` 数据流**）→ 板级 DTS 中 **`syscon`/`syscon-*` 引用**。
