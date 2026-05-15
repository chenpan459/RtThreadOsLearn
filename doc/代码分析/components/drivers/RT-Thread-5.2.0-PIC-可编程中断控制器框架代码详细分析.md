# RT-Thread 5.2.0 PIC（可编程中断控制器）框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/pic` 目录实现的 **PIC 抽象层**：在设备模型（**`RT_USING_DM`**）与 OpenFirmware 设备树（**`RT_USING_OFW`**）之上，为 **ARM GICv2/v1、GICv3** 及可选的 **GICv2m（MSI 帧）**、**GICv3 ITS** 提供统一的中断注册、分发、级联与 **MSI** 相关钩子。对外头文件为 **`components/drivers/include/drivers/pic.h`**；**`rtdevice.h`** 在 **`RT_USING_PIC`** 下包含 **`drivers/pic.h`**。

---

## 1. 模块定位与设计目标

### 1.1 解决的问题

- **统一逻辑 IRQ 编号**：将各控制器的 **硬件 IRQ（hwirq）** 映射到内核使用的 **`int irq`**（与 **`MAX_HANDLERS`** 上限内的 **`struct rt_pic_irq`** 槽位一一对应）。
- **与 OFW 对齐**：设备树 **`interrupts`** / **`interrupt-parent`** 解析后，通过 **`rt_pic_ops::irq_parse` + `irq_map`** 完成映射（见 **`components/drivers/ofw/irq.c`** 中的 **`ofw_map_irq`**）。
- **替代/封装传统 `rthw` 中断接口**：**`pic_rthw.c`** 将 **`rt_hw_interrupt_*`** 转发到 **`rt_pic_*`**，使旧驱动仍可用 **`rt_hw_interrupt_install`** 等 API。
- **SMP IPI**：在 GIC 公共代码里为 **`RT_SCHEDULE_IPI` / `RT_STOP_IPI` / `RT_SMP_CALL_IPI`** 预留 **IPI 专用槽位**（**`_ipi_hash`**），与 SPI/PPI 线性区分离。

### 1.2 与其它子系统的关系

| 子系统 | 关系 |
|--------|------|
| **`drivers/core`（DM）** | Kconfig **`depends on RT_USING_DM`**；PIC 常作为 **`struct rt_device` + `struct rt_pic`** 组合体的一部分嵌入（见 **`pic.h`** 注释）。 |
| **`drivers/ofw`** | **`RT_PIC_OFW_DECLARE`** 展开为 **`RT_OFW_STUB_EXPORT`**；**`rt_pic_init`** 遍历带 **`interrupt-controller`** 属性的节点并 **`rt_ofw_stub_probe_range`**。 |
| **`drivers/ofw/irq.c`** | **`rt_ofw_get_irq`** → **`ofw_map_irq`** 要求中断父节点的 **`rt_ofw_data`** 能 **`rt_pic_dynamic_cast`** 为 **`struct rt_pic`**，且 **`ops`** 必须实现 **`irq_parse`/`irq_map`**。 |
| **`drivers/pci`（MSI）** | **`RT_PIC_ARM_GIC_V2M`** / **`RT_PIC_ARM_GIC_V3_ITS`** 依赖 **`RT_PCI_MSI`**；在 **`rt_pic_ops`** 中提供 **`irq_compose_msi_msg`/`irq_write_msi_msg`/`irq_alloc_msi`/`irq_free_msi`** 等可选成员。 |

---

## 2. Kconfig 选项（`pic/Kconfig`）

| 选项 | 含义 |
|------|------|
| **`RT_USING_PIC`** | 总开关；**`select RT_USING_ADT`、`RT_USING_ADT_BITMAP`**；**`depends on RT_USING_DM`** |
| **`RT_USING_PIC_STATISTICS`** | ISR 执行时间统计；依赖 **`RT_USING_KTIME`**、**`RT_USING_INTERRUPT_INFO`** |
| **`MAX_HANDLERS`** | 逻辑 IRQ 与 **`_pirq_hash`** 数组上界（默认 **256**） |
| **`RT_PIC_ARM_GIC`** | 编译 **GICv2/v1**（**`pic-gicv2.c`**）；**`select RT_USING_OFW`** |
| **`RT_PIC_ARM_GIC_V2M`** | **GICv2m** MSI 帧（**`pic-gicv2m.c`**）；需 **`RT_PIC_ARM_GIC` && `RT_PCI_MSI`** |
| **`RT_PIC_ARM_GIC_V3`** | **GICv3**（**`pic-gicv3.c`**）；**`select RT_USING_OFW`** |
| **`RT_PIC_ARM_GIC_V3_ITS`** | **ITS**（**`pic-gicv3-its.c`**）；**`select RT_USING_ADT_REF`** |
| **`RT_PIC_ARM_GIC_V3_ITS_IRQ_MAX`** | ITS 使用的 IRQ 数量上限（与 **`ARCH_CPU_64BIT`** 默认值相关） |
| **`RT_PIC_ARM_GIC_MAX_NR`** | 平台可同时存在的 **GICv2 实例个数**（**`SOC_REALVIEW`** 默认 **2**，否则 **1**） |

---

## 3. `SConscript` 与源文件编排

在 **`RT_USING_PIC`** 关闭时整组不加入编译。

- **始终编译**：**`pic.c`**（核心框架）、**`pic_rthw.c`**（**`rthw` 适配**）。
- **`RT_PIC_ARM_GIC` 或 `RT_PIC_ARM_GIC_V3`**：**`pic-gic-common.c`**（GIC 公共 quirk、电平/边沿配置、SGI/IPI 辅助等）。
- **`RT_PIC_ARM_GIC`**：**`pic-gicv2.c`**。
- **`RT_PIC_ARM_GIC_V2M`**：**`pic-gicv2m.c`**。
- **`RT_PIC_ARM_GIC_V3`**：**`pic-gicv3.c`**。
- **`RT_PIC_ARM_GIC_V3_ITS`**：**`pic-gicv3-its.c`**。

头文件 **`pic-gic*.h`** 由对应 **`.c`** 包含，不单独在 **`SConscript`** 中列出。

---

## 4. 核心数据结构（`pic.h`）

### 4.1 `struct rt_pic`

- **`parent`**：**`struct rt_object`**，类型常为 **`RT_Object_Class_Unknown`** 或嵌入在 **`rt_device` 之后**，供 **`rt_pic_dynamic_cast`** 识别（配合默认名 **`"PIC"`**）。
- **`list`**：挂入全局 **`_pic_nodes`** 链表，供 **`rt_pic_irq_init`/`rt_pic_irq_finit`** 遍历调用各 **`irq_init`/`irq_finit`**。
- **`ops`**：虚函数表 **`struct rt_pic_ops`**。
- **`irq_start` / `irq_nr` / `pirqs`**：本 PIC 在全局 **`_pirq_hash`** 中占用的 **连续子区间** 及 **`rt_pic_irq` 数组基址**（由 **`rt_pic_linear_irq`** 分配）。

### 4.2 `struct rt_pic_ops`

关键回调（节选）：

| 成员 | 作用 |
|------|------|
| **`irq_init`/`irq_finit`** | 控制器级上电/下电初始化（在 **`rt_pic_irq_init`** 阶段统一调用）。 |
| **`irq_enable/disable/mask/unmask/ack/eoi`** | 对单条 **`struct rt_pic_irq`** 的硬件操作。 |
| **`irq_set_priority`/`irq_set_affinity`/`irq_set_triger_mode`** | 动态属性；未实现时 **`rt_pic_irq_*`** 返回 **`-RT_ENOSYS`**。 |
| **`irq_send_ipi`** | SMP 下向 **`cpumask`** 发 **SGI**。 |
| **`irq_compose_msi_msg`/`irq_write_msi_msg`/`irq_alloc_msi`/`irq_free_msi`** | PCI MSI 与 PIC 协同（**GICv2m/ITS** 路径使用）。 |
| **`irq_set_state`/`irq_get_state`** | 查询/设置 **PENDING/ACTIVE/MASKED**（宏 **`RT_IRQ_STATE_*`**）。 |
| **`irq_map`/`irq_parse`** | **OFW 映射路径强制要求**（见下文 **`ofw/irq.c`**）。 |
| **`flags`** | 如 **`RT_PIC_F_IRQ_ROUTING`**：级联时在父 **`children_nodes`** 上挂子节点，**`rt_pic_handle_isr`** 可沿子链分发。 |

### 4.3 `struct rt_pic_irq`

- **`irq`**：逻辑编号；**`hwirq`**：控制器侧编号（如 GIC **IntID**）。
- **`mode`**：**边沿/电平**（**`RT_IRQ_MODE_*`**）。
- **`affinity`**：**CPU 位图**（**`RT_IRQ_AFFINITY_*`** 宏封装）。
- **`isr`**：主 **`struct rt_pic_isr`** + 链表节点 **`list`**，支持 **同一 IRQ 多处理函数**（**`rt_pic_attach_irq`** 首项用内嵌结构，后续 **`rt_malloc`** 追加）。
- **`parent`**：**级联 PIC** 的父 **`rt_pic_irq`**（**`rt_pic_cascade`**）。
- **`pic`**：所属 **`struct rt_pic`**。
- **`msi_desc`**：**PCI MSI** 描述符反向指针（**v2m** 等路径）。

### 4.4 宏 **`RT_PIC_OFW_DECLARE(name, ids, handler)`**

等价于 **`RT_OFW_STUB_EXPORT(name, ids, pic, handler)`**，把 **GIC 等 probe 函数** 注册到 **pic** 类的 OFW stub 区间，供 **`rt_pic_init`** 中的 **`rt_ofw_stub_probe_range`** 调用。

---

## 5. `pic.c`：全局 IRQ 表与运行时路径

### 5.1 全局状态

- **`_pirq_hash[MAX_HANDLERS]`**：所有逻辑 IRQ 的 **`struct rt_pic_irq`** 静态存储；前面若干槽位预留给 **IPI**（**`_ipi_hash`** 与 **`_pirq_hash_idx`** 配合 **`rt_pic_linear_irq`** 切片分配）。
- **`_pic_nodes`**：已注册的 **`struct rt_pic`** 链表。
- **`_pic_lock`**：**`rt_pic_linear_irq`/`rt_pic_cancel_irq`/名称长度** 等全局元数据保护。
- **`_traps_nodes`**：**`rt_pic_add_traps`** 注册的 **“陷阱”处理链**；**`rt_pic_do_traps`** 在关中断语义下依次尝试，用于 **Spurious 或平台特殊中断** 等扩展。

### 5.2 IRQ 号解析

- **`irq2pirq(int irq)`**：范围检查 + **`pirq->irq >= 0`** 才视为有效（**`rt_pic_config_irq`/`rt_pic_config_ipi`** 会写入 **`irq`/`hwirq`/`pic`**）。

### 5.3 分配与配置

- **`rt_pic_linear_irq(pic, irq_nr)`**：在 **`_pirq_hash`** 尾部划出 **`irq_nr`** 个槽位，设置 **`pic->irq_start`/`irq_nr`/`pirqs`**，默认对象名 **`"PIC"`**，并入 **`_pic_nodes`**；溢出返回 **`-RT_EEMPTY`**。
- **`rt_pic_config_irq` / `rt_pic_config_ipi`**：把 **`pic->irq_start + irq_index`** 或固定 **IPI 下标** 与 **`hwirq`** 绑定到 **`_pirq_hash`** 对应项。

### 5.4 级联

- **`rt_pic_cascade(pirq, parent_irq)`**：设置 **`parent`**，可选把 **`pirq->list`** 挂到父的 **`children_nodes`**（当 **`RT_PIC_F_IRQ_ROUTING`** 置位时）。
- **`rt_pic_uncascade`**：对称拆除。

### 5.5 ISR 安装与执行

- **`rt_pic_attach_irq`/`rt_pic_detach_irq`**：基于 **`uid`（param）** 匹配；支持链式多 ISR。
- **`rt_pic_handle_isr(pirq)`**：
  - 若存在 **`children_nodes`**：对每个子 **`irq_ack` → 递归 `rt_pic_handle_isr` → `irq_eoi`**。
  - 再执行本节点主 **`isr.action`** 及链表上额外 **`rt_pic_isr`**。
  - 可选 **`RT_USING_PIC_STATISTICS`**：用 **`ktime`** 统计 **`min/max/sum`** 耗时。

### 5.6 对外的 **`rt_pic_irq_*` 系列**

均通过 **`irq2pirq`** 找到 **`pirq`**，在 **`pirq->rw_lock`** 下调用 **`pic->ops`** 对应成员；**`rt_pic_irq_parent_*`** 则对 **`pirq->parent`** 再调一层（供 **子 PIC / MSI 中间层** 委托父 GIC）。

### 5.7 初始化入口 **`rt_pic_init`**

```1142:1174:rt-thread-5.2.0/components/drivers/pic/pic.c
#ifdef RT_USING_OFW
RT_OFW_STUB_RANGE_EXPORT(pic, _pic_ofw_start, _pic_ofw_end);

static rt_err_t ofw_pic_init(void)
{
    struct rt_ofw_node *ic_np;

    rt_ofw_foreach_node_by_prop(ic_np, "interrupt-controller")
    {
        rt_ofw_stub_probe_range(ic_np, &_pic_ofw_start, &_pic_ofw_end);
    }

    return RT_EOK;
}
#else
static rt_err_t ofw_pic_init(void)
{
    return RT_EOK;
}
#endif /* !RT_USING_OFW */

rt_err_t rt_pic_init(void)
{
    rt_err_t err;

    LOG_D("init start");

    err = ofw_pic_init();

    LOG_D("init end");

    return err;
}
```

含义：在 **OFW 已展开设备树** 的前提下，对所有 **`interrupt-controller`** 节点尝试匹配已注册的 **PIC stub**（如 **GICv2/GICv3**），执行各 **`*_ofw_init`**：映射寄存器、填充 **`struct gicv2`/`struct gicv3`**、**`rt_pic_linear_irq`**、注册 **trap handler**（见 GIC 节）等。

**注意**：仓库内 **`rt_pic_init()`** 的显式调用出现在 **`libcpu/aarch64/common/setup.c`**（FDT 反扁平化之后）；其它架构若启用 PIC，需在板级或 CPU 启动路径中自行保证 **先 `rt_pic_init` 再 `rt_pic_irq_init`** 的顺序。

---

## 6. `pic_rthw.c`：与 `rthw` 的薄封装

```17:78:rt-thread-5.2.0/components/drivers/pic/pic_rthw.c
void rt_hw_interrupt_init(void)
{
    /* initialize pic */
    rt_pic_irq_init();
}

void rt_hw_interrupt_mask(int vector)
{
    rt_pic_irq_mask(vector);
}

void rt_hw_interrupt_umask(int vector)
{
    rt_pic_irq_unmask(vector);
}

rt_isr_handler_t rt_hw_interrupt_install(int vector, rt_isr_handler_t handler,
        void *param, const char *name)
{
    rt_pic_attach_irq(vector, handler, param, name, RT_IRQ_F_NONE);

    return RT_NULL;
}

void rt_hw_interrupt_uninstall(int vector, rt_isr_handler_t handler, void *param)
{
    rt_pic_detach_irq(vector, param);
}

#if defined(RT_USING_SMP) || defined(RT_USING_AMP)
void rt_hw_ipi_send(int ipi_vector, unsigned int cpu_mask)
{
    RT_BITMAP_DECLARE(cpu_masks, RT_CPUS_NR) = { cpu_mask };

    rt_pic_irq_send_ipi(ipi_vector, cpu_masks);
}

void rt_hw_ipi_handler_install(int ipi_vector, rt_isr_handler_t ipi_isr_handler)
{
    /* note: ipi_vector maybe different with irq_vector */
    rt_hw_interrupt_install(ipi_vector, ipi_isr_handler, 0, "IPI_HANDLER");
}
#endif
```

要点：**`rt_hw_interrupt_init` 不再直接碰 GIC 寄存器**，而是假定 **OFW probe（或其它路径）已完成 PIC 注册**，此处仅 **第二轮** 调用各 **`irq_init`**。在 **AArch64 + PIC** 的 **`setup.c`** 中，**`rt_pic_init` 与 `rt_pic_irq_init` 被直接连续调用**，**`rt_hw_interrupt_init` 可能根本不会走到**——两种入口需按平台启动代码理解，避免重复或遗漏初始化。

---

## 7. `pic-gic-common.*`：GIC 共享逻辑

- **`gic_common_init_quirk_ofw` / `gic_common_init_quirk_hw`**：按 **compatible** 或 **IIDR 掩码** 执行芯片级 workaround（如 **PL390 字节访问问题**）。
- **`gic_common_sgi_config`**：在 **`irq_base` 较小时** 为 SMP 调用 **`rt_pic_config_ipi`**，把 **调度/停止/SMP call** 三类 IPI 绑定到 **hwirq 0..2** 区段（与 **`cpuport.h`** 中 IPI 编号约定一致）。
- **`gic_common_configure_irq`**：写 **GICD_ICFGR** 区域配置边沿/电平；带 **`sync_access`** 回调处理需要 **DSB/特殊同步** 的平台。
- **`gic_common_dist_config`/`gic_common_cpu_config`**：分发器与 CPU interface 的通用使能、优先级掩码等（被 v2/v3 复用）。
- **`gic_fill_ppi_affinity`**：把 **PPI** 绑定到 **本 CPU 可见集合**。

**`pic-gic-common.h`** 还声明 **`gicv2m_ofw_probe`**、**`gicv3_its_ofw_probe`**，供主 GIC 在 **OFW init** 末尾按需拉起 **MSI/ITS** 子节点。

---

## 8. `pic-gicv2.c` / `pic-gicv2.h`：GICv2/v1

### 8.1 对象与寄存器

**`struct gicv2`** 内嵌 **`struct rt_pic parent`**，并保存 **`dist_base`/`cpu_base`**（及可选 **Hyp/VCPU** 映射）。**`pic-gicv2.h`** 定义 **GICD/GICC** 偏移常量。

### 8.2 初始化概要

- **`gicv2_dist_init`**：读 **`GIC_DIST_TYPE`** 得 **`max_irq`**（上限 **1020**），配置 **SPI target**、**优先级、组、使能** 等；支持 **`skip-init`** 设备树属性跳过破坏固件已设状态。
- **`gicv2_init`**：**`rt_pic_linear_irq`** 分配 **`max_irq + 1 - GIC_SGI_NR`** 条逻辑线（**SGI 0..15** 不占线性表索引，IPI 走 **`rt_pic_find_ipi`**）；**`gic_common_sgi_config`**；**`rt_pic_add_traps(gicv2_handler, gic)`**。

### 8.3 中断分发 **`gicv2_handler`**

从 **`GICC_IAR`** 取 **hwirq**；**1020–1023** 视为 **Spurious**；**`<16`** 走 **IPI**；否则 **`rt_pic_find_irq`**。顺序：**`irq_ack` → `rt_pic_handle_isr` → `irq_eoi`**（与 **EOI split** 模式下的 **DIR** 寄存器配合，代码中通过 **`_gicv2_eoi_mode_ns`** 等分支处理）。

### 8.4 OFW 映射

- **`gicv2_irq_parse`**：标准 **3 cell**（**type, irq, flags**）；**SPI**：**`hwirq = args[1] + 32`**；**PPI**：**`+16`**。
- **`gicv2_irq_map`**：**`irq_index = hwirq - GIC_SGI_NR`**，设置默认 **priority/affinity/mode**，调用 **`rt_pic_config_irq`**；非默认电平时写 **ICFGR**。

### 8.5 多实例

静态数组 **`_gicv2_list[RT_PIC_ARM_GIC_MAX_NR]`**，**`_gicv2_nr`** 递增；**`rt_ofw_data(np) = &gic->parent`** 供 **`rt_ofw_get_irq`** 查找 **PIC**。

**`RT_PIC_ARM_GIC_V2M`** 时，在 **GICv2 版本号为 2** 时调用 **`gicv2m_ofw_probe`** 扫描 **MSI 帧**子节点。

---

## 9. `pic-gicv3.c` / `pic-gicv3.h`：GICv3

### 9.1 与 v2 的主要差异

- **单全局 **`struct gicv3 _gic`****（典型单簇 GICv3 平台）；**Redistributor** 按 CPU **`rt_hw_cpu_id()`** 选择 **`redist_percpu_base[]`**。
- **CPU interface** 通过 **系统寄存器接口**（**`read_gicreg`/`write_gicreg`** 封装 **`ICC_*`**）。
- **扩展 IntID 类型**：**ESPI、EPPI、LPI**（**8192** 起）；**`gicv3_irq_parse`** 对 **cell[0]** 的分支更丰富；**`gicv3_irq_map`** 对 **LPI** 使用 **`irq_index = irq_nr - lpi_nr + hwirq - 8192`** 映射到 **`rt_pic_linear_irq`** 预留的尾部区间。

### 9.2 其它特性

- **RWP（寄存器写挂起）** 轮询：**`gicv3_wait_for_rwp`**。
- **Erratum / Quirk**：如 **MSM8996 GICR_WAKER**、**ARM64 2941627** 等以静态标志控制。
- **Trap**：**`gicv3_handler`** 读 **`ICC_IAR1`**，分支与 v2 类似，增加 **LPI 索引** 计算。

### 9.3 ITS / v2m 探测

在 **`gicv3_ofw_init`** 末尾：若编译 **ITS**，先 **`gicv3_its_ofw_probe`**；否则在 **PCI MSI** 开启时回退尝试 **`gicv2m_ofw_probe`**（兼容部分 DT 描述方式）。对应 **`RT_PIC_OFW_DECLARE(gicv3, ...)`**。

---

## 10. `pic-gicv2m.c`：GICv2m MSI 帧

- **`struct gicv2m`** 内嵌 **`struct rt_pic parent`**，并记录 **MSI 帧 MMIO**、**SPI 区间**（**`MSI_TYPER`**）、平台 **flags**（如 **Graviton 仅地址写**）。
- **`gicv2m_irq_mask/unmask`**：组合 **`rt_pci_msi_mask_irq`** 与 **`rt_pic_irq_parent_*`**，把 **MSI 实际 SPI** 仍交给 **父 GIC**。
- **`irq_compose_msi_msg`**：按 **v2m** 规范填充 **PCI `msi_msg`**（地址/数据）。
- **`gicv2m_ofw_probe`**：解析 **`msi-controller`** 类节点，**`rt_pic_linear_irq`** 分配 **向量位图**，**`rt_pic_cascade`** 到父 **SPI**，向 **`rt_ofw_data`** 注册本 **PIC** 供 MSI 分配路径查找。

---

## 11. `pic-gicv3-its.c`：GICv3 ITS

文件体量较大，职责包括：

- **ITS 表结构**（**Device/Device table/EventID→Collection** 等）与 **命令队列** 编程。
- **LPI** 与 **PCI MSI/MSI-X** 映射到 **ITS 中断号**，再通过父 **GICv3** 的 **LPI 支持** 与 **`irq_map`** 衔接。
- **`gicv3_its_ofw_probe`**：由 **`pic-gicv3.c`** 在 **OFW init** 阶段调用，完成 **ioremap**、**初始化命令序列**、与 **`struct gicv3`** 的 **`lpi_nr`** 等字段协同。

**`RT_PIC_ARM_GIC_V3_ITS_IRQ_MAX`** 控制 **ITS 侧 IRQ 资源上界**，需与 **`rt_pic_linear_irq`** 为 **LPI 预留的槽位数** 一致规划。

---

## 12. 与 `ofw/irq.c` 的衔接（设备树 → 逻辑 IRQ）

```525:563:rt-thread-5.2.0/components/drivers/ofw/irq.c
static int ofw_map_irq(struct rt_ofw_cell_args *irq_args)
{
    int irq;
    struct rt_ofw_node *ic_np = irq_args->data;
    struct rt_pic *pic = rt_pic_dynamic_cast(rt_ofw_data(ic_np));

    /* args.data is "interrupt-controller" */
    if (pic)
    {
        struct rt_pic_irq pirq;

        if (!pic->ops->irq_parse)
        {
            LOG_E("Master pic MUST implemented irq_parse");
            RT_ASSERT(0);
        }

        if (!pic->ops->irq_map)
        {
            LOG_E("Master pic MUST implemented irq_map");
            RT_ASSERT(0);
        }

        irq = pic->ops->irq_parse(pic, irq_args, &pirq);

        if (!irq)
        {
            irq = pic->ops->irq_map(pic, pirq.hwirq, pirq.mode);
        }
    }
    else
    {
        LOG_E("Master pic %s not support", ic_np->full_name);
        irq = -RT_EIO;
    }

    rt_ofw_node_put(ic_np);

    return irq;
}
```

要点：

- **`irq_parse` 返回 0** 表示成功，随后用 **`out_pirq->hwirq`/`mode`** 调 **`irq_map`** 得到 **逻辑 `irq`**；非 0 时 **`irq`** 直接为错误码（与 **`rt_err_t`** 混用 **`int`** 通道，阅读 GIC 实现时需注意 **`RT_EOK == 0`**）。
- 若 **`interrupt-controller` 节点** 未在 **`rt_pic_init`** 阶段挂上 **`rt_ofw_data`**（未匹配 stub），**`rt_pic_dynamic_cast`** 失败，映射报错。

**`rt_ofw_get_irq`** 在映射成功后还可根据 **`interrupt-affinity`** 调 **`rt_pic_irq_set_affinity`**。

---

## 13. 引导时序示例（AArch64）

```286:297:rt-thread-5.2.0/libcpu/aarch64/common/setup.c
    cpu_info_init();

#ifdef RT_USING_PIC
    rt_pic_init();
    rt_pic_irq_init();
#else
    /* initialize hardware interrupt */
    rt_hw_interrupt_init();

    /* initialize uart */
    rt_hw_uart_init();
#endif
```

在 **`RT_USING_PIC`** 路径下：**FDT `rt_fdt_unflatten` 之后** 先 **probe 所有 PIC**，再统一执行各 **`irq_init`**（使能分发器、CPU interface、陷阱向量等）。**UART 初始化** 在该片段中落在 **`#else`**，实际产品若仅开启 PIC，通常会在其它位置初始化控制台，需结合完整 **`setup.c`** 阅读。

---

## 14. 调试与运维（`pic.c` 末尾）

在 **`RT_USING_CONSOLE` && `RT_USING_MSH`** 下注册 **`list_irq`** 等命令（源码后半段），可列出已占用 **IRQ** 的 **mode、affinity、handler 名** 等，便于现场排查 **映射与 ISR 挂接** 问题。

---

## 15. 小结

| 层次 | 文件/组件 | 职责 |
|------|-----------|------|
| 抽象 | **`pic.h` / `pic.c`** | **逻辑 IRQ 表**、**ISR 链**、**级联**、**全局 init**、**`rthw` 无关的 PIC API** |
| 适配 | **`pic_rthw.c`** | **`rt_hw_interrupt_*` / `rt_hw_ipi_*`** 转发 |
| GIC 公共 | **`pic-gic-common.*`** | **Quirk、ICFGR、SGI/IPI、PPI affinity** |
| GICv2 | **`pic-gicv2.*`** | **Distributor/CPU IF MMIO**、**OFW 多 compatible** |
| GICv3 | **`pic-gicv3.*`** | **GICD/GICR、系统寄存器 CPU IF、LPI/ESPI** |
| MSI | **`pic-gicv2m.c`**、**`pic-gicv3-its.c`** | **MSI 帧 / ITS** 与 **PCI MSI** 框架对接 |

阅读或移植时建议顺序：**`pic.h` 数据结构 → `pic.c` 映射与 `handle_isr` → `ofw/irq.c` 映射调用点 → 选定 `pic-gicv2` 或 `pic-gicv3` 的 `*_ofw_init` 与 `*_handler`**。

---

*文档对应源码树版本：RT-Thread 5.2.0；路径前缀：`rt-thread-5.2.0/components/drivers/pic/`。*
