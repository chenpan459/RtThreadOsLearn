# RT-Thread 5.2.0 PHY 以太网物理层框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/phy` 目录：提供 **两代互斥** 的以太网 **PHY** 抽象——**旧版 `RT_USING_PHY`**（**`rt_mdio_bus` + `rt_phy_ops` + 字符设备读写寄存器`**）与 **新版 `RT_USING_PHY_V2`**（**`mii_bus` + `rt_phy_driver` + Linux 风格 MII/Clause 45、genphy、OFW 辅助**）。公共头文件为 **`components/drivers/include/drivers/phy.h`**；**`RT_USING_PHY_V2`** 时 **`phy.h`** 还会包含 **`phy/`** 目录下的 **`ofw.h`、`mdio.h`、`general_phy.h`**（通过 **`SConscript` CPPPATH`** 指向 **`components/drivers/phy`**）。

**`rtdevice.h`**：**`RT_USING_PHY`** 时 **`#include "drivers/phy.h"`**（与 **`RT_USING_PHYE`** 的 **`phye.h`** 不同子系统，勿混）。

---

## 1. Kconfig 与编译

| 选项 | 含义 |
|------|------|
| **`RT_USING_PHY`** | 旧版 PHY；**`default n`** |
| **`RT_USING_PHY_V2`** | 新版 PHY + MDIO；**`depends on !RT_USING_PHY`**（**二选一**） |

**`SConscript`**：

- 默认 **`Glob('*.c')`** 取 **`general.c`、`mdio.c`、`ofw.c`、`phy.c`**。
- **无 `RT_USING_OFW`**：移除 **`ofw.c`**。
- **无 `RT_USING_PHY_V2`**：移除 **`general.c`、`mdio.c`、`ofw.c`**。
- **既无 V2 又无 `RT_USING_PHY`**：再移除 **`phy.c`**（即两开关都关时本组无源文件）。

**`CPPPATH`**：**`phy` 目录 + `../include`**。

---

## 2. 旧版 PHY（`RT_USING_PHY`，`phy.h` 后半段 + `phy.c` 前半段）

### 2.1 数据结构

- **`struct rt_mdio_bus_ops`**：**`init/read/write/uninit`**，**`read/write`** 以 **`void *data` + `size`** 传递 **32 位寄存器值** 等。
- **`struct rt_mdio_bus`**：**`hw_obj`、`name`、`ops`**。
- **`struct rt_phy_ops`**：**`init`、`read`、`write`、`loopback`、`get_link_status`、`get_link_speed_duplex`**。
- **`struct rt_phy_device`**：**`rt_device parent`、`bus`、`addr`、`ops`**；**`rt_phy_msg`**：**`reg`/`value`** 供 **`read`/`write`** 设备接口使用。

### 2.2 `phy.c`（`#ifdef RT_USING_PHY`）

**`rt_hw_phy_register`**：设备类型 **`RT_Device_Class_PHY`**，**`read`/`write`** 转 **`bus->ops->read/write(phy->addr, msg->reg, &msg->value, 4)`**，由以太网驱动或应用通过 **设备名** 访问 PHY 寄存器。

---

## 3. 新版 PHY V2（`RT_USING_PHY_V2`，`phy.h` 前半段 + `phy.c` 后半段等）

### 3.1 `mdio.h` / `mdio.c` — **`struct mii_bus`**

- **回调**：**`read(bus, addr, devad, reg)`**、**`write`**；可选 **`read_c45`/`write_c45`**、**`reset`**。
- **管理**：**`phymap[32]`**（按 PHY 地址索引 **`rt_phy_device *`**）、**`phy_mask`**、**`reset_delay_us`** 等。
- **API**：**`rt_mdio_alloc`**、**`rt_mdio_register/unregister`**、**`rt_mdio_get_bus_by_name`**；**`INIT_CORE_EXPORT(mdio_init)`** 初始化全局 **`mdio_list`**。

### 3.2 `struct rt_phy_device`（V2）

继承 **`rt_device`**：**`mii_bus *`、`rt_phy_driver *drv`、`phy_id`、`speed/duplex/link`、`advertising/supported`、`autoneg`、Clause 45 标记 **`is_c45`**、**`interface`**（**`rt_phy_interface`**）、**`flags`**、**`priv`**；**`RT_USING_OFW`** 时 **`struct rt_ofw_node *node`**。

### 3.3 `struct rt_phy_driver`

继承 **`rt_driver`**：**`name`、`uid/mask`（PHY ID 匹配）**、**`mmds`、`features`**（**`RT_SUPPORTED_*`** 位图）；**`probe/config/startup/shutdown`**；**`read/write`**（可覆盖默认 MDIO）；**`read_mmd/write_mmd`**（Clause 45 / MMD）。

### 3.4 `phy.c`（V2 部分摘要）

- **`rt_phy_read/write`**：按 **`is_c45`** 选 **`read_c45`** 或 **`read`**。
- **`rt_phy_read_mmd/write_mmd`**：驱动提供 **`read_mmd`** 则直调；否则 **10G 特征或 devad 特殊** 时退化为 **Clause 22**；否则 **`rt_phy_mmd_start_indirect`** + 读 **`MII_MMD_DATA`**。
- **`rt_phy_reset`**：**`BMCR.RESET`** 轮询清除；**`RT_PHY_FLAG_BROKEN_RESET`** 时跳过。
- **`rt_phy_device_create`**：**`rt_malloc`** 清零，**`rt_phy_device_register`**，并在合法 **addr** 且非 **FIXED_ID/NCSI_ID** 时填入 **`bus->phymap[addr]`**。
- **`rt_phy_startup/config/shutdown`**：转 **`drv`** 对应回调。
- **`rt_phy_bus`**：**`struct rt_bus`**，用于 **PHY 驱动注册/匹配**（与 **`rt_phy_driver_register`** 配合，细节在 **`phy.c`** 后部）。

**宏**：**`RT_PHY_DEVICE_REGISTER` / `RT_PHY_DRIVER_REGISTER`** 使用 **`INIT_PREV_EXPORT`** 自动注册。

---

## 4. Genphy（`general.c` + `general_phy.h`）

实现 **IEEE 802.3 Clause 22** 常见 **自协商/链路解析** 逻辑，与 Linux **genphy** 思路类似：

- **`rt_genphy_config_aneg`**、**`rt_genphy_update_link`**、**`rt_genphy_startup`**、**`rt_genphy_config`**、**`rt_genphy_parse_link`** 等（**`general_phy.h`** 声明）。
- 使用 **`RT_MII_*`、`RT_BMCR_*`、`RT_ADVERTISE_*`** 等常量（**`general_phy.h`** 大量 MII 位定义）。

---

## 5. OFW 辅助（`ofw.c` + `phy/ofw.h`）

**依赖 `RT_USING_OFW` 且 V2**（否则 **`ofw.c`** 不编）。

- **`rt_phy_modes[]`**：与 **`rt_phy_interface`** 枚举一一对应的 **字符串**（**`rgmii`、`sgmii`** 等），供 **`phy-mode`/`phy-connection-type`** 属性解析。
- **`rt_ofw_get_interface`**：读 **`phy-mode`**，若无则 **`phy-connection-type`**，再 **`_get_interface_by_name`**。
- **`rt_ofw_get_mac_addr` / `_by_name`**：依次尝试 **`mac-address`、`local-mac-address`、`address`**。
- **`rt_ofw_get_phyid`**：解析 **`compatible`** 为 **`ethernet-phy-idXXXX.YYYY`** 格式得到 **32 位 PHY OUI+模型**（依赖 **`rt_sscanf`**）。
- **`rt_ofw_create_phy`**：**`phy-handle`** phandle 找 **PHY 节点**；**`rt_ofw_get_phyid(np,&id)`** 中的 **`np` 为调用方以太网节点**（与 **`phy-handle` 子节点** 分离，使用场景需与 DTS 编写约定一致）；再 **`rt_phy_device_create`** 并记录 **`node`**。

**`phy.h`** 另声明 **`rt_phy_get_device`**（组合 **bus / OFW / interface** 的工厂函数，见 **`phy.c`**）。

---

## 6. 头文件包含关系注意点

**`phy.h`** 在 **V2** 下写 **`#include <ofw.h>`、`<mdio.h>`、`<general_phy.h>``**，依赖 **`components/drivers/phy`** 在 **include 路径** 中；**若第三方模块只 `-I include` 而不含 `phy` 目录**，可能找不到 **`mdio.h`**——应以 **`#include <rtdevice.h>`** 或工程 **`CPPPATH`** 与官方 **`SConscript`** 一致为准。

---

## 7. 小结

| 维度 | 旧版 `RT_USING_PHY` | 新版 `RT_USING_PHY_V2` |
|------|---------------------|-------------------------|
| MDIO | **`rt_mdio_bus` + ops 四件套** | **`mii_bus` + read/write + C45** |
| PHY | **`rt_phy_ops` 绑定单器件** | **`rt_phy_driver` + PHY ID + genphy** |
| 设备 | **寄存器 **`rt_phy_msg`** 字符通道** | **`rt_phy_device` 为 `rt_device` + 总线驱动模型** |
| DT | 无本目录 **`ofw.c`** | **`phy-mode`、MAC 地址、`phy-handle`** |

阅读顺序：**`phy.h`** 分清 **`#ifdef`** 两段 → **`mdio.c`/`phy.c`(V2)** → **`general.c`** → **`ofw.c`**。
