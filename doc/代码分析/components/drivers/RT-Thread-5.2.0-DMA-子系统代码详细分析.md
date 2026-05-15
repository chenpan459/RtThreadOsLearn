# RT-Thread 5.2.0 DMA 子系统代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/dma` 目录实现的 **DMA 控制器抽象**（通道、传输准备、设备树绑定）与 **DMA 内存池 / 映射与缓存同步**（`dma_pool.c`）。SoC 专用 DMAC 驱动通常在 **`$(SOC_DM_DMA_DIR)`**（由 `Kconfig` **`osource`** 引入）中实现 **`rt_dma_controller_ops`**。

涉及文件：

- 控制器与通道：`dma.c`
- 内存池与 `rt_dma_alloc` 族：`dma_pool.c`
- 对外 API：`components/drivers/include/drivers/dma.h`（`rtdevice.h` 在 **`RT_USING_DMA`** 下包含）
- 配置与构建：`Kconfig`、`SConscript`

---

## 1. 依赖与编译

| 项 | 说明 |
|----|------|
| **`RT_USING_DMA`** | **`depends on RT_USING_DM`**，并 **`select RT_USING_ADT`**、**`select RT_USING_ADT_BITMAP`** |
| **`SConscript`** | 固定编译 **`dma.c`、`dma_pool.c`**，`CPPPATH` 指向 **`components/drivers/include`** |
| **SoC** | **`osource "$(SOC_DM_DMA_DIR)/Kconfig"`** 用于板级 DMAC 选项 |

---

## 2. 数据类型（`dma.h`）

### 2.1 传输方向 `enum rt_dma_transfer_direction`

**MEM_TO_MEM / MEM_TO_DEV / DEV_TO_MEM / DEV_TO_DEV**，以及 **`RT_DMA_DIR_MAX`** 作为位图上限。

### 2.2 `struct rt_dma_slave_config`

方向、源/目的 **总线宽度**（字节枚举）、**`src_addr`/`dst_addr`**（从机侧 DMA 可访问窗口/设备 FIFO 物理基址语义由驱动约定）、**burst** 与 **port window** 等。

### 2.3 `struct rt_dma_slave_transfer`

**`src_addr`/`dst_addr`、`buffer`、`dma_handle`、`buffer_len`、`period_len`**（周期 DMA 用）。

### 2.4 `struct rt_dma_controller`

- **`dev`**：关联 **`struct rt_device`**（一般为 DMAC 设备）。
- **`dir_cap`**：位图，表示支持的方向（**`rt_dma_controller_add_direction`** 置位）。
- **`ops`**：**`struct rt_dma_controller_ops`**。
- **`channels_nodes`**：本控制器已分配通道链表。
- **`mutex`**：控制器级互斥，保护 **config/prep/start/stop** 等与 **`ops`** 的交互。

### 2.5 `struct rt_dma_controller_ops`

| 回调 | 作用 |
|------|------|
| **`request_chan` / `release_chan`** | 分配/释放 **`struct rt_dma_chan`**；缺省时框架 **`rt_calloc`** 默认通道 |
| **`start` / `stop`** | 启停一次已 prepare 的传输 |
| **`config`** | 按 **`rt_dma_slave_config`** 配置硬件 |
| **`prep_memcpy`** | 内存拷贝描述符 |
| **`prep_cyclic`** | 环形缓冲音频等场景 |
| **`prep_single`** | 单次散列缓冲 |

### 2.6 `struct rt_dma_chan`

保存 **`name`（`dma-names` 匹配串）**、**`ctrl`/`slave`**、**`conf_err`/`prep_err`**、缓存的 **`conf`/`transfer`**、完成回调 **`callback`**、**`priv`**。

### 2.7 内存侧：`struct rt_dma_pool`、`struct rt_dma_map_ops`

- **Pool**：物理 **`region`**、页级 **bitmap 分配器**、**`flags`**（**`RT_DMA_F_*`**）、可选 **`dev`**（设备专用池）。
- **`rt_dma_map_ops`**：**`alloc/free/sync_out_data/sync_in_data`**，可由 **`rt_dma_device_set_ops`** 或 OFW **`memory-region`** 自动挂 **`ofw_dma_map_ops`**。

---

## 3. 控制器生命周期（`dma.c`）

### 3.1 `rt_dma_controller_register`

校验 **`ctrl`、`ctrl->dev`、`ctrl->ops`**；**`dir_cap`** 至少一个方向有效；生成 **`"%s-dmac"`** 风格名用于 **`rt_mutex_init`**；将 **`ctrl->list`** 插入全局 **`dmac_nodes`**（**`dmac_nodes_lock`** 保护）；初始化 **通道链表** 与 **mutex**；若 **`dev->ofw_node`** 存在则 **`rt_dm_dev_bind_fwdata(dev, NULL, ctrl)`** 便于 **`rt_ofw_data`** 反查控制器。

### 3.2 `rt_dma_controller_unregister`

**mutex** 下若 **通道链表非空** 返回 **`-RT_EBUSY`****；解绑 fwdata；摘链、**`rt_mutex_detach`**。

### 3.3 `rt_dma_chan_start/stop`

若 **`prep_err`** 非 0 直接返回（表示尚未成功 prepare）；否则在 **`ctrl->mutex`** 下调用 **`ops->start/stop`**。

### 3.4 `rt_dma_chan_config`

校验方向与 **总线宽度枚举**；检查 **`dir_cap`**；**非 MEM_TO_MEM** 时要求 **`chan->name` 非空**（命名通道）；**mutex** 下 **`ops->config`**，成功则 **`memcpy` 保存 `chan->conf`**，并 **`chan->conf_err`** 记录结果。

### 3.5 `rt_dma_chan_done`

若 **`chan->callback`** 非空则调用，供 ISR 报告传输完成量。

### 3.6 地址合法性 **`range_is_illegal`**

当前实现：**当 `addr0 < addr1` 视为 illegal**（打日志）。在 **`prep_memcpy`** 中用于 **`transfer` 地址 ≥ `conf` 中配置的 `src_addr`/`dst_addr`** 的**下界**检查；**未检查上界窗口**，完整区间约束需驱动或上层保证。

### 3.7 `rt_dma_prep_memcpy / prep_cyclic / prep_single`

- 若 **`conf_err`** 非 0 直接返回。
- **cyclic/single**：按方向选择检查 **`src_addr` 或 `dst_addr`** 与 **`conf`** 下界；**DEV_TO_DEV** 路径 **`dma_buf_addr = ~0UL`** 传入 **`prep_cyclic`**（依赖驱动解释）。
- **mutex** 下调用对应 **`prep_*`**；成功则缓存 **`transfer`**，**`prep_err`** 记录错误码。

### 3.8 `rt_dma_chan_request(dev, name)`

- **`name` 非空（OFW）**：**`ofw_find_dma_controller`**：按 **`dma-names`** 找索引，解析 **`dmas` + `#dma-cells`** phandle，**`rt_platform_ofw_request`** 触发控制器节点 probe，**`ctrl = rt_ofw_data(ctrl_np)`**。
- **`name` 为空**：遍历 **`dmac_nodes`**，选**第一个支持 MEM_TO_MEM** 的控制器（隐式「未命名」仅 M2M 场景）。
- 调 **`ops->request_chan`** 或 **`rt_calloc` 默认 `rt_dma_chan`**；初始化链表、**`conf_err/prep_err = -RT_ERROR`**，插入 **`ctrl->channels_nodes`**。

### 3.9 `rt_dma_chan_release`

从链表摘除；若 **`ops->release_chan`** 存在则调用，否则 **`rt_free(chan)`**。

---

## 4. DMA 内存池与映射（`dma_pool.c`）

### 4.1 全局 **`dma_pool_nodes`** + **`dma_pools_lock`**

所有已安装 **`struct rt_dma_pool`** 通过 **`list`** 串联。

### 4.2 默认 `rt_dma_map_ops`：coherent vs nocoherent

- **`dma_map_coherent_ops`**：**`sync_out`**：`rt_kmem_v2p` + **`rt_hw_cpu_dcache_ops(FLUSH)`**；**`sync_in`**：**`INVALIDATE`**。
- **`dma_map_nocoherent_ops`**：仅 **`v2p`**，不做 cache 操作。

### 4.3 `device_dma_ops(dev)`

1. 若 **`dev->dma_ops`** 已设，直接返回。
2. **OFW**：**`ofw_device_dma_ops`** 解析 **`memory-region`** phandle 列表，为每个 region **`dma_pool_install`**，并按 **`dma-coherent` 属性** 与 **`no-map`**（见下）设置 **`pool->flags`**，返回 **`ofw_dma_map_ops`**。
3. 否则：若设备树 **`dma-coherent`** 为真选 **coherent**，否则 **nocoherent**，并缓存到 **`dev->dma_ops`**。

### 4.4 OFW 路径 **`ofw_dma_map_ops`**

- **`alloc/free`**：在通用 **`dma_alloc/dma_free`** 基础上对 **`dma_handle`** 做 **`rt_ofw_translate_cpu2dma` / `dma2cpu`**。
- **`sync_*`**：按 **`RT_DMA_F_NOCACHE`** 选择 coherent/nocoherent 分支后再做地址翻译。

**阅读注意**：**`ofw_device_dma_ops`** 中在 **`rt_ofw_node_put(mem_np)`** 之后仍调用 **`rt_ofw_prop_read_bool(mem_np, "no-map")`**，存在 **已释放节点上读属性** 的风险，审阅 BSP/上游时可对照修复。

### 4.5 **`dma_pool_install` / `rt_dma_pool_install`**

将 **`rt_region_t`** 对齐到 **`ARCH_PAGE_SIZE`**，按页数分配 **bitmap**，插入 **`dma_pool_nodes`**；默认置 **`RT_DMA_F_LINEAR`**；若 region 上界 **< 4GB** 则置 **`RT_DMA_F_32BITS`**。

### 4.6 **`dma_pool_alloc` / `dma_pool_free`**

在 bitmap 中寻找 **连续空闲页**；**`dma_alloc`** 遍历各 pool，按 **`RT_DMA_F_DEVICE`（设备专用池）**、**`NOMAP`、`32BITS`、`LINEAR`、`NOCACHE`** 与 pool 标志匹配；有映射需求时 **`rt_ioremap_nocache/cached`**，失败则释放 bitmap 并重试下一 pool。

### 4.7 **`rt_dma_alloc` / `rt_dma_free` / `sync_*`**

**`device_dma_ops`** 分派；若 ops 无 **`alloc`** 则走内部 **`dma_alloc`**。**`rt_dma_alloc_coherent`** 为 **`NOCACHE|LINEAR`** 的封装。

### 4.8 **`rt_dma_pool_extract`**

从 **`region_list`** 中挑选可容纳 **`cma_size`** 的低 4G 区域（必要时退而求其次 **>4G**），切出 **`coherent_pool_size`** 安装为 **`coherent-pool`**，剩余 **`cma`** 区域再安装为 **`cma`** 池（注释：**CMA > coherent-pool** 指地址布局顺序）。

### 4.9 **`dma_free` 与 `RT_DMA_F_NOMAP`**

**`NOMAP`** 时 **`dma_alloc`** 可能直接返回 **物理地址作 `cpu_addr`**，但 **`dma_free`** 仍无条件 **`rt_iounmap(cpu_addr)`**。使用 **NOMAP** 路径时需与当前实现语义核对，避免对非映射虚拟地址 **`iounmap`**。

---

## 5. 标志位（`dma.h`）

| 宏 | 含义 |
|----|------|
| **`RT_DMA_F_LINEAR`** | 线性物理连续区域（pool 默认） |
| **`RT_DMA_F_32BITS`** | 位于 4G 以下（pool install 自动推断） |
| **`RT_DMA_F_NOCACHE`** | 非缓存映射 / sync 走 nocoherent 分支 |
| **`RT_DMA_F_DEVICE`** | 该 pool 绑定特定 **`dev`** |
| **`RT_DMA_F_NOMAP`** | 不做 **`ioremap`**，CPU 视角与 handle 语义由平台定义 |

---

## 6. MSH

**`list_dma_pool`**：打印各 pool 的 **`region.name`** 与 **`[start,end]`**。

---

## 7. BSP / 驱动对接流程（摘要）

1. 填充 **`struct rt_dma_controller`**：**`dev`、`dir_cap`、`ops`**。
2. **`rt_dma_controller_register(ctrl)`**。
3. 实现 **`request_chan`**（解析 **`fw_data` → `rt_ofw_cell_args`**）、**`config`、`prep_*`、`start/stop`** 等。
4. 传输完成在 ISR 中 **`rt_dma_chan_done(chan, residue_or_size)`**。
5. 需要一致性缓冲区时，从设备侧 **`rt_dma_alloc(dev, ...)`** 或预装 **`rt_dma_pool_install`**。

---

## 8. 小结

| 维度 | 内容 |
|------|------|
| **`dma.c`** | **DMAC 注册**、**通道请求（DT `dmas/dma-names`）**、**config/prep/start**、**回调触发** |
| **`dma_pool.c`** | **预留物理区 + 页位图分配**、**`rt_dma_alloc/free/sync`**、**OFW `memory-region`/`dma-coherent`** |
| 依赖 | **DM + ADT 位图**；头文件依赖 **MMU/页大小** 等 |

阅读顺序：**`dma.h`**（契约）→ **`dma.c`**（控制面）→ **`dma_pool.c`**（缓冲与一致性）→ 对照 SoC **`drv_dma*.c`** 中的 **`rt_dma_controller_ops`** 实现。
