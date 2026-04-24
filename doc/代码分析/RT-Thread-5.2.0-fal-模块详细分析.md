# RT-Thread 5.2.0 `components/fal` 模块详细分析

**FAL（Flash Abstraction Layer，Flash 抽象层）** 把多片、多类型的 Flash 统一成 **「逻辑 Flash 设备」**，再在其上划分 **「命名分区」**，向应用与上层组件提供 **按分区名读写擦除** 的能力，并可一键生成 **块设备 / 字符设备 / MTD NOR 设备**，便于挂载 **FatFs**、跑 **OTA / EasyFlash / FlashDB** 等。

本文档基于 `rt-thread-5.2.0/components/fal` 源码与配置，说明数据模型、初始化流程、分区表两种模式、与 SFUD/DFS 的关系及 API 要点。

---

## 1. 目录与编译

| 路径 | 说明 |
|------|------|
| `inc/fal.h` | 对外 API 声明（依赖 **`fal_cfg.h`**） |
| `inc/fal_def.h` | `fal_flash_dev`、`fal_partition` 结构体定义 |
| `src/fal.c` | `fal_init()`：依次初始化 Flash 表与分区表 |
| `src/fal_flash.c` | Flash 设备表遍历、`fal_flash_device_find` |
| `src/fal_partition.c` | 分区表装载/查找、`fal_partition_*` 读写擦 |
| `src/fal_rtt.c` | 对接 `rt_device`：块设备、可选 MTD NOR、字符设备 |
| `samples/porting/` | **`fal_cfg.h` 模板**、STM32 片内 Flash 示例、**SFUD 端口** |
| `docs/fal_api.md` / `fal_api_en.md` | 官方 API 说明（中英文） |

**`SConscript`**：

- 默认编译 `src/*.c`。  
- 若 **`FAL_USING_SFUD_PORT`**：额外编译 **`samples/porting/fal_flash_sfud_port.c`**（把 SFUD 封装成一颗 `fal_flash_dev`）。  
- 整组依赖 **`RT_USING_FAL`**，`CPPPATH` 含 `inc/`。

**注意**：`fal.h` 中 **`#include <fal_cfg.h>`** 必须由 **BSP 或工程** 提供路径（通常放在 `board` 或 `ports` 目录），否则会编译失败；其中宏 **`FAL_FLASH_DEV_TABLE`** 在 `fal_flash.c` 中若未定义会直接 **`#error`**。

---

## 2. 核心数据模型

### 2.1 Flash 设备 `struct fal_flash_dev`（`fal_def.h`）

表示一颗「可被 FAL 管理的」Flash，可以是 **片内 Flash**、**SPI Nor**、**并口 Nor** 等，由 BSP 提供 **`ops`**：

| 字段 | 含义 |
|------|------|
| `name` | 逻辑名，供分区表 `flash_name` 引用 |
| `addr` / `len` | 在该设备坐标系下的起始与总长度（具体语义与 `ops` 实现一致，常见为片内 0 起算或整片 SPI） |
| `blk_size` | **擦除粒度**（erase sector/block），块设备几何会用到 |
| `ops.init/read/write/erase` | 底层驱动；`read/write/erase` 在 `fal_flash_init` 中会被 **RT_ASSERT** 检查非空 |
| `write_gran` | **写粒度（位）**：Nor 常为 1；部分 STM32 片内为 8/32/64 等，用于告知上层按字对齐写入 |
| `blocks[]` | 可选 **多块描述**（`flash_blk`：size × count），用于日志打印与部分复杂布局；最多 `FAL_DEV_BLK_MAX`（默认 6） |

所有 `fal_flash_dev` 指针放入 **`FAL_FLASH_DEV_TABLE`** 宏展开的静态表，由 **`fal_flash_init()`** 逐个 `ops.init()`。

### 2.2 分区 `struct fal_partition`

| 字段 | 含义 |
|------|------|
| `magic_word` | 固定 **`FAL_PART_MAGIC_WORD`（0x45503130）**；编译期表与 Flash 内嵌表均用此魔数 |
| `name` | 分区逻辑名，如 `easyflash`、`download` |
| `flash_name` | 指向 **`fal_flash_dev.name`** |
| `offset` / `len` | 在该 Flash 设备上的 **绝对偏移** 与 **长度** |

**地址换算**：`fal_partition_read/write/erase` 中的参数 **`addr` 为相对分区起始的偏移**，内部调用：

```text
flash_dev->ops.xxx(part->offset + addr, ...)
```

并检查 **`addr + size <= part->len`**，防止越界。

---

## 3. 初始化流程

```text
fal_init()
  ├── fal_flash_init()     // 校验 ops，调用各 flash 的 init，标记 flash 层 init_ok
  └── fal_partition_init() // 装载分区表，绑定 part → flash_dev 缓存，返回分区个数
```

- 成功且首次成功时 **`fal.c`** 打日志「Flash Abstraction Layer initialize success」。  
- **`fal_init_check()`**（在 `fal.c`）：返回是否已成功初始化（供其他模块判断）。

分区个数 **`> 0`** 时 `fal_init` 认为分区阶段成功；具体返回值以 `fal_partition_init` 实现为准（编译期表为表项数；从 Flash 动态加载失败可能为 0）。

---

## 4. 分区表的两种来源（`Kconfig`）

### 4.1 `FAL_PART_HAS_TABLE_CFG`（默认 y）

- 在 **`fal_cfg.h`** 中定义 **`FAL_PART_TABLE`** 宏，展开为 **`static const struct fal_partition[]`**。  
- 每条分区必须带合法 **`magic_word`**（见 `samples/porting/fal_cfg.h` 示例）。  
- **优点**：简单、可版本管理、不依赖 Flash 上是否已有表。  
- **缺点**：改分区要改固件重新编译。

### 4.2 关闭 `FAL_PART_HAS_TABLE_CFG`

- 上电后从 **指定 Flash 设备** 上、以 **`FAL_PART_TABLE_END_OFFSET`** 为 **扫描上边界**，**向前** 滑动读取，在数据中搜索魔数 **`0x45503130`**，找到后按 **`struct fal_partition`** 连续项读入 **`rt_malloc`/`rt_realloc`** 得到运行时表。  
- 需在 **`fal_cfg.h`** 提供 **`FAL_PART_TABLE_FLASH_DEV_NAME`** 与 **`FAL_PART_TABLE_END_OFFSET`**（Kconfig 可填默认）。  
- **适用**：分区由量产工具或 booloader 写入 Flash 固定区域，应用与 FAL 仅「发现」表。  
- **风险**：若 Flash 无合法表或魔数损坏，初始化失败；需额外 Flash 空间与烧录流程配合。

---

## 5. 与 RT-Thread 设备的衔接（`fal_rtt.c`）

在 **`fal_partition_*`** 之上，FAL 可把某分区包装成标准 **`rt_device`**，供 **DFS elm-FatFs**（`diskio` 绑块设备名）、**MTD**、或 **裸读写字符流** 使用。

### 5.1 `fal_blk_device_create(partition_name)`

- 分配 **`fal_blk_device`**，挂 **`rt_device_blk_geometry`**：`bytes_per_sector` 取 **`fal_flash_dev->blk_size`**。  
- **`read`/`write`**：以扇区为单位换算到 **`fal_partition_read/write`**。  
- **`write`** 路径中先对逻辑区间 **`fal_partition_erase`** 再 **`fal_partition_write`**（与 Nor 按扇区写入习惯一致）。  
- **`RT_DEVICE_CTRL_BLK_ERASE`**：把扇区范围换算为字节后调用 **`fal_partition_erase`**。  

创建后需 **`rt_device_register`**（实现里会注册，具体见 `fal_rtt.c` 后半段）。

### 5.2 `fal_mtd_nor_device_create(partition_name)`

- 在定义 **`RT_USING_MTD_NOR`** 时编译。  
- 将分区暴露为 **MTD Nor** 设备，便于走 MTD 子系统或特定协议栈。

### 5.3 `fal_char_device_create(partition_name)`

- 字符设备形式暴露分区，适合 **顺序大块读写**、简单下载等（非块扇区语义时需注意与 `blk_size` 对齐策略）。

**参数名拼写**：对外 API 使用 **`parition_name`**（少字母 e），为历史拼写，调用时按头文件即可。

---

## 6. SFUD 端口（`FAL_USING_SFUD_PORT`）

开启 **`FAL_USING_SFUD_PORT`** 后编译 **`fal_flash_sfud_port.c`**：

- 定义全局 **`struct fal_flash_dev nor_flash0`**（名默认 **`FAL_USING_NOR_FLASH_DEV_NAME`**，常配 **`norflash0`**）。  
- **`init()`**：在 **`RT_USING_SFUD`** 下通过 **`rt_sfud_flash_find_by_dev_name`** 取得 **`sfud_flash_t`**，并用芯片 **`erase_gran`**、**`capacity`** 更新 **`blk_size`/`len`**。  
- **`read/write/erase`**：调用 **`sfud_read` / `sfud_write` / `sfud_erase`**，offset 加上 **`nor_flash0.addr`**（通常为 0）。

因此：**SFUD 负责 SPI Flash 芯片协议与容量发现**，**FAL 负责分区与对上层统一命名**。

---

## 7. Kconfig 选项小结

| 选项 | 作用 |
|------|------|
| **`RT_USING_FAL`** | 总开关 |
| **`FAL_USING_DEBUG`** | 调试日志（默认在 `RT_USING_DEBUG` 时为 y） |
| **`FAL_PART_HAS_TABLE_CFG`** | 分区表是否在 `fal_cfg.h` 静态定义 |
| **`FAL_PART_TABLE_FLASH_DEV_NAME`** / **`FAL_PART_TABLE_END_OFFSET`** | 动态搜表时的设备名与扫描终点偏移 |
| **`FAL_USING_SFUD_PORT`** | 编入 SFUD 适配的 `nor_flash0` |
| **`FAL_USING_NOR_FLASH_DEV_NAME`** | SFUD 端口使用的 RT 设备名（默认 `norflash0`） |

---

## 8. 典型使用场景

1. **KV / 日志**：EasyFlash、FlashDB 等通过 **`fal_partition_find("xxx")` + `fal_partition_*`** 访问。  
2. **文件系统**：`fal_blk_device_create("filesystem")` 得到块设备，再在 **`dfs_mkfs` / `dfs_mount`** 或 elm diskio 中绑定该设备名。  
3. **OTA**：download 分区与 app 分区分离，下载与校验写入 download，再由 bootloader 搬运。  
4. **多 Flash**：`FAL_FLASH_DEV_TABLE` 中并列多颗 `fal_flash_dev`，分区表通过 **`flash_name`** 指向不同设备。

---

## 9. 移植检查清单

1. 在工程中提供 **`fal_cfg.h`**：`#include` 板级头文件，定义 **`FAL_FLASH_DEV_TABLE`**；按是否静态表定义 **`FAL_PART_TABLE`** 或 **`FAL_PART_TABLE_*`**。  
2. 实现或引用各 **`fal_flash_dev`** 的 **`ops`**（片内可参考 `fal_flash_stm32f2_port.c`，SPI Nor 可开 **`FAL_USING_SFUD_PORT`**）。  
3. 在合适的 **`INIT_APP_EXPORT`/`INIT_COMPONENT_EXPORT`** 阶段调用 **`fal_init()`**（具体顺序需早于使用该分区的组件）。  
4. 确认 **`write_gran`**、**`blk_size`** 与芯片手册一致，避免 FatFs **`RT_DFS_ELM_MAX_SECTOR_SIZE`** 等与真实擦除大小不一致导致异常。

---

## 10. 相关文档与交叉引用

- 组件内 API 手册：`components/fal/docs/fal_api.md`（中文）、`fal_api_en.md`  
- 移植说明：`components/fal/samples/README.md`、`samples/porting/README.md`  
- 与 DFS / 块设备关系：`doc/RT-Thread-5.2.0-dfs-模块详细分析.md`、`doc/RT-Thread-5.2.0-drivers-模块详细分析.md`

---

*文档对应源码树：`rt-thread-5.2.0/components/fal`（5.2.0）。*
