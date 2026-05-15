# RT-Thread 5.2.0 HWCRYPTO 硬件密码框架代码详细分析

本文面向源码阅读，说明 `rt-thread-5.2.0/components/drivers/hwcrypto` 目录实现的 **硬件加解密抽象层**：统一 **`rt_hwcrypto_device`** 注册与 **`rt_hwcrypto_ctx`** 生命周期，并在其上提供 **对称算法、GCM、HASH、RNG、CRC、大数运算** 等面向应用的 API。具体密码引擎由 BSP 在 **`create/destroy/copy/reset`** 中挂接硬件，并填充各子模块的 **`ops`** 指针。

应用侧通过 **`rtdevice.h` → `drivers/crypto.h`** 一次性包含 **`hwcrypto.h` 与各 `hw_*.h`**。

---

## 1. 文件与编译关系

| 源文件 | 条件（`SConscript`） |
|--------|----------------------|
| **`hwcrypto.c`** | 始终编译 |
| **`hw_symmetric.c`** | `RT_HWCRYPTO_USING_AES` 或 **DES/3DES/RC4** 任一为 y |
| **`hw_gcm.c`** | 在上述对称开启且 **`RT_HWCRYPTO_USING_GCM`** |
| **`hw_hash.c`** | **MD5 / SHA1 / SHA2** 任一为 y |
| **`hw_rng.c`** | **`RT_HWCRYPTO_USING_RNG`** |
| **`hw_crc.c`** | **`RT_HWCRYPTO_USING_CRC`** |
| **`hw_bignum.c`** | **`RT_HWCRYPTO_USING_BIGNUM`** |

**`CPPPATH`**：`hwcrypto` 目录 + 工程根（`Dir('#')`），以便 **`#include <hwcrypto.h>`** 等。

头文件均位于 **`components/drivers/hwcrypto/`** 下，与 **`crypto.h`** 配合暴露给 **`rtdevice`**。

---

## 2. Kconfig 要点

- **`RT_USING_HWCRYPTO`**：总开关。
- **`RT_HWCRYPTO_DEFAULT_NAME`**：默认设备名字符串，**默认 `"hwcryto"`**（与 **`hwcrypto.h`** 中 fallback 拼写一致，历史笔误）。
- **`RT_HWCRYPTO_IV_MAX_SIZE` / `RT_HWCRYPTO_KEYBIT_MAX_SIZE`**：IV 与密钥最大位数（对称 **`setkey`/`setiv`** 校验用）。
- 子选项按算法族细分：**AES 模式**、**DES/3DES**、**RC4**、**SHA2 宽度**、**CRC 多项式**、**BIGNUM 运算子集** 等，与 **`SConscript`** 编译裁剪一致。

---

## 3. 类型体系（`hwcrypto.h`）

### 3.1 `hwcrypto_type`

- **主类型**：高 16 位（**`HWCRYPTO_MAIN_TYPE_MASK`**），通过 **`(__LINE__ - HWCRYPTO_TYPE_HEAD) & 0xffff` 再左移 16 位** 编码 **AES/DES/3DES/RC4/GCM/MD5/SHA1/SHA2/RNG/CRC/BIGNUM** 等。  
  **注意**：依赖 **`__LINE__`** 的枚举值对**源码行号变化敏感**，合并分支时需谨慎 diff。
- **子类型**：**`HWCRYPTO_SUB_TYPE_MASK`**（如 AES-ECB/CBC、SHA224/256 等）。

### 3.2 `hwcrypto_mode`

**`HWCRYPTO_MODE_ENCRYPT` / `DECRYPT` / `UNKNOWN`**。

### 3.3 `struct rt_hwcrypto_ops`（设备级）

| 回调 | 作用 |
|------|------|
| **`create(ctx)`** | 按 **`ctx->type`** 分配/初始化硬件上下文 **`ctx->contex`**（指针字段名 **`contex`** 为历史拼写） |
| **`destroy`** | 释放硬件上下文 |
| **`copy`** | 硬件状态在上下文之间复制 |
| **`reset`** | 复位硬件状态 |

### 3.4 `struct rt_hwcrypto_device`

继承 **`rt_device`**（注册为 **`RT_Device_Class_Security`**），含 **`ops`、`id`、`user_data`**。`read/write/control` 在 **`hwcrypto.c`** 中可为空（**`RT_USING_DEVICE_OPS`** 下全 **`RT_NULL`**），**控制面一般由各算法 API + `create` 内逻辑完成**。

### 3.5 `struct rt_hwcrypto_ctx`

**`device`、`type`、`contex`**：所有具体上下文结构体均 **把本结构放在首字段**（即 **嵌入继承**）。

---

## 4. 核心实现（`hwcrypto.c`）

- **`rt_hwcrypto_set_type`**：仅允许 **主类型相同** 或从 **`HWCRYPTO_TYPE_NULL`** 初始化。
- **`rt_hwcrypto_ctx_init`**：绑定设备、**`set_type`**、调用 **`device->ops->create`**。
- **`rt_hwcrypto_ctx_create`**：**`rt_malloc(obj_size)`**，**`obj_size` 不得小于 `sizeof(struct rt_hwcrypto_ctx)`**，失败路径释放内存。
- **`rt_hwcrypto_ctx_destroy`**：**`destroy`** 后 **`rt_free`**。
- **`rt_hwcrypto_ctx_cpy`**：要求 **同一 `device` 且主类型相同**，再调 **`ops->copy`**。
- **`rt_hwcrypto_dev_default`**：静态缓存 **`rt_device_find(RT_HWCRYPTO_DEFAULT_NAME)`** 并转为 **`rt_hwcrypto_device*`**。
- **`rt_hwcrypto_register`**：**`RT_ASSERT`** 要求 **`ops` 及 `create/destroy/copy/reset` 非空**，**`rt_device_register(..., RT_DEVICE_FLAG_RDWR)`**。

---

## 5. 对称密码（`hw_symmetric.c` + `hw_symmetric.h`）

### 5.1 `struct hwcrypto_symmetric`

在 **`rt_hwcrypto_ctx`** 之上增加：

- **`flags`**：**`SYMMTRIC_MODIFY_KEY/IV/IVOFF`**，在 **`setkey`/`setiv`/`set_ivoff`** 时置位；**`crypt` 成功后清除**（供驱动判断是否需要重配硬件）。
- **`key[]` / `iv[]`、`key_bitlen`、`iv_len`、`iv_off`**。
- **`hwcrypto_symmetric_ops`**：仅 **`crypt(symmetric_ctx, symmetric_info)`**。

### 5.2 主要 API

- **`rt_hwcrypto_symmetric_create/destroy`**：**`sizeof(struct hwcrypto_symmetric)`** 创建。
- **`rt_hwcrypto_symmetric_crypt`**：组 **`hwcrypto_symmetric_info`**（mode/in/out/length），调 **`ops->crypt`**。
- **`setkey`**：拷贝密钥，**`bitlen <= RT_HWCRYPTO_KEYBIT_MAX_SIZE`**，置 **`MODIFY_KEY`**。
- **`setiv`**：**`len <= RT_HWCRYPTO_IV_MAX_SIZE`**，置 **`MODIFY_IV`**。
- **`symmetric_cpy`**：拷贝软件侧 key/iv/flags 等后 **`rt_hwcrypto_ctx_cpy`**。
- **`symmetric_reset`**：清零软件缓冲后 **`rt_hwcrypto_ctx_reset`**。

---

## 6. GCM（`hw_gcm.c` + `hw_gcm.h`）

- **`struct hwcrypto_gcm`**：**首字段为 `struct hwcrypto_symmetric parent`**，另含 **`crypt_type`**（底层块算法类型）与 **`hwcrypto_gcm_ops`**（**`start(add)`、`finish(tag)`**）。
- **`rt_hwcrypto_gcm_create`**：**`HWCRYPTO_TYPE_GCM`**，记录 **`crypt_type`**。
- **`rt_hwcrypto_gcm_crypt`**：直接转调 **`rt_hwcrypto_symmetric_crypt`**。
- **Key/IV**：转调对称 **`rt_hwcrypto_gcm_setkey` 等**。
- **`gcm_cpy`**：拷贝 **`crypt_type`** 后 **`rt_hwcrypto_symmetric_cpy`**。

---

## 7. HASH（`hw_hash.c` + `hw_hash.h`）

- **`struct hwcrypto_hash`**：**`hwcrypto_hash_ops`** 提供 **`update`、`finish`**。
- API：**`create/destroy`、`update`、`finish`、`cpy`、`reset`、`set_type`**（`set_type` 调 **`rt_hwcrypto_set_type`**）。

---

## 8. RNG（`hw_rng.c` + `hw_rng.h`）

- 模块静态 **`ctx_default`**。
- **`rt_hwcrypto_rng_default(device)`**：销毁旧默认上下文，**`rng_create`** 新上下文并设为默认；**`device==NULL`** 时清除默认。
- **`rt_hwcrypto_rng_update()`**：若默认空则 **`rng_default(rt_hwcrypto_dev_default())`**，再 **`ops->update`**。

---

## 9. CRC（`hw_crc.c` + `hw_crc.h`）

- **`rt_hwcrypto_crc_create(device, mode)`**：**`HWCRYPTO_TYPE_CRC`**，按 **`hwcrypto_crc_mode`** 选择 **`HWCRYPTO_CRC8_CFG`** 等预设 **`crc_cfg`**。
- **`rt_hwcrypto_crc_update`**：调 **`ops->update`**，返回 **CRC 值**。
- **`rt_hwcrypto_crc_cfg`**：覆盖 **`crc_cfg`**。

---

## 10. 大数（`hw_bignum.c` + `hw_bignum.h`）

- 静态 **`bignum_default`** 上下文；**`hwcrypto_bignum_dev_is_init`** 在未设置时 **`rt_hwcrypto_bignum_default(rt_hwcrypto_dev_default())`**。
- **`rt_hwcrypto_bignum_*`**：**`add/sub/mul/mulmod/exptmod`** 均转发到 **`hwcrypto_bignum_ops`**（若存在）。
- **`hw_bignum_mpi`**：**`sign/total/p`**，配套 **`init/free`、`get_len`、`export_bin/import_bin`**（大端字节序）。

---

## 11. BSP 驱动对接要点

1. 实现 **`struct rt_hwcrypto_ops`**：在 **`create`** 中根据 **`ctx->type`** 分配硬件 session，设置 **`ctx->contex`**，并把 **`hwcrypto_symmetric.ops`**（或 hash/rng 等）指向驱动静态表。
2. **`destroy`** 释放硬件资源；**`copy/reset`** 与芯片语义对齐。
3. **`rt_hwcrypto_register(&drv_device, "与 Kconfig 一致的名字")`**，保证 **`rt_hwcrypto_dev_default()`** 能 **`find`** 到（或使用 **`rt_hwcrypto_rng_default` / `bignum_default`** 显式绑定）。
4. **对称 `crypt`**：根据 **`symmetric_ctx->flags`** 判断是否需要把 **`key/iv`** 写入寄存器。

---

## 12. 小结

| 项目 | 说明 |
|------|------|
| 架构 | **设备 + 通用 ctx + 各算法扩展结构体 + 子 ops** |
| 入口头 | **`drivers/crypto.h`** 聚合 **`hwcrypto` + 各算法头** |
| 裁剪 | **Kconfig + SConscript** 双控制，未开启的算法源文件不编译 |
| 注意 | **`hwcrypto_type` 基于 `__LINE__`**；**`contex`/`hwcryto` 拼写**；**`rt_hwcrypto_gcm_getkey` 声明与对称 `getkey` 返回类型不一致**（实现上为 int 转用） |

阅读顺序：**`hwcrypto.h`**（类型与设备 ops）→ **`hwcrypto.c`**（注册与 ctx）→ 按需深入 **`hw_symmetric.c`** 及其它算法文件 → 对照 BSP **`drv_hwcrypto`** 或 SoC 安全子系统驱动。
