# RT-Thread 5.2.0 MAILBOX 硬件邮箱框架代码详细分析

本文说明 `rt-thread-5.2.0/components/drivers/mailbox` 目录实现的 **Mailbox（硬件邮箱）抽象**：在 **DM + OFW** 场景下，消费设备通过 **`mboxes` / `mbox-names`** 解析 **控制器与通道索引**，经 **`rt_mbox_controller_ops`** 完成 **request/send/peek/release**；软件层用 **每通道 `rt_timer` 单次定时** 可选地约束 **`send` 异步完成** 超时。参考实现 **`mailbox-pic.c`** 提供 **`rt-thread,pic-mailbox`** 与 **PIC 中断** 联动的 **32 通道** 虚拟硬件模型。

涉及文件：

- **`mailbox.c`**：控制器注册、通道请求、**`send`/`send_done`/`recv`**、默认 **`#mbox-cells`** 解析
- **`mailbox-pic.c`**：**PIC mailbox** 平台驱动
- **`Kconfig`、`SConscript`**
- 头文件：**`components/drivers/include/drivers/mailbox.h`**（**`rtdevice.h`** 在 **`RT_USING_MBOX`** 下包含）

---

## 1. 模块定位与依赖

```text
消费设备驱动（struct rt_mbox_client + dev->ofw_node）
    ↓
rt_mbox_request_by_index / by_name
    ↓
rt_ofw_parse_phandle_cells("mboxes", "#mbox-cells", ...)
    ↓
struct rt_mbox_controller + chans[index]
    ↓
ops->send / peek / request / release
    ↓
SoC 硬件邮箱 或 pic-mailbox MMIO
```

| Kconfig | 含义 |
|---------|------|
| **`RT_USING_MBOX`** | 总开关；**`depends on RT_USING_DM`、`RT_USING_OFW`** |
| **`RT_MBOX_PIC`** | 编译 **`mailbox-pic.c`**（默认 **y**） |
| **`osource "$(SOC_DM_MBOX_DIR)/Kconfig"`** | SoC 可外接扩展 |

**`SConscript`**：无 **`RT_USING_MBOX`** 直接返回；否则必编 **`mailbox.c`**，**`RT_MBOX_PIC`** 时加 **`mailbox-pic.c`**。

---

## 2. 数据结构（`mailbox.h`）

### 2.1 `struct rt_mbox_controller`

- **`list`**：挂入全局 **`mbox_nodes`** 链表（加 **`mbox_ops_lock`**）
- **`dev`**：**`struct rt_device *`**（一般为 **platform `parent`**）
- **`ops`**：**`const struct rt_mbox_controller_ops *`**
- **`num_chans`**：通道数量
- **`chans`**：**`rt_calloc(num_chans, sizeof(struct rt_mbox_chan))`**

### 2.2 `struct rt_mbox_controller_ops`

| 成员 | 必选 | 说明 |
|------|------|------|
| **`request`** | 可选 | 申请通道（如清 **IMASK**） |
| **`release`** | 建议 | 释放通道 |
| **`send`** | 建议 | 发送 **`data`**（语义由硬件定义，PIC 实现假定 **`rt_uint32_t *`**） |
| **`peek`** | 可选 | 非阻塞探测 |
| **`ofw_parse`** | 可选 | 从 **`rt_ofw_cell_args`** 解析通道下标；缺省见 **`mailbox.c`** |

### 2.3 `struct rt_mbox_chan`

- **`ctrl` / `client`**
- **`data`**：**`send` 成功后** 暂存 **`const void *`**，供 **`send_done`** 传给 **`tx_done`**
- **`complete`**：**`send` 是否已结束**（硬件完成或超时 **`send_done`**）
- **`timer`**：**单次**定时器，超时回调 **`mbox_chan_timeout` → `rt_mbox_send_done(ETIMEOUT)`**
- **`lock`**：保护本通道 **`tx_prepare`/`send`/定时器** 相关字段
- **`priv`**：控制器私有

### 2.4 `struct rt_mbox_client`

- **`dev`**：须带 **`ofw_node`**（用于 **`mboxes`**）
- **`rx_callback`**：**`rt_mbox_recv`** 调用
- **`tx_prepare` / `tx_done`**：**`send` 前后钩子**（**`tx_done`** 在 **`rt_mbox_send_done`** 中调用）

---

## 3. 核心逻辑（`mailbox.c`）

### 3.1 `rt_mbox_controller_register`

- 校验 **`ctrl`、`dev`、`ops`、`num_chans`**
- **`rt_calloc`** **`chans`**；对每个通道：**`chan->ctrl`**、**`spin_lock_init`**、**`rt_timer_init(..., ONE_SHOT, mbox_chan_timeout, chan)`**，定时器名 **`"<devname>-<i>"`**
- **`rt_dm_dev_bind_fwdata(dev, RT_NULL, ctrl)`**（将 **`ctrl`** 绑到 **`dev->ofw_node`** 的 **`rt_ofw_data`**）
- **`mbox_nodes`** 链表头后插入 **`ctrl->list`**

### 3.2 `rt_mbox_controller_unregister`

**`rt_dm_dev_unbind_fwdata`**，从链表摘除；**`i` 从 `num_chans-1` 到 `0`** 依次 **`rt_mbox_release(&ctrl->chans[i])`**；**`rt_free(chans)`**。

### 3.3 `rt_mbox_send`

1. 关 **`chan->lock`**
2. 若有 **`client->tx_prepare`**
3. **`chan->complete = RT_FALSE`**，**`err = ops->send(chan, data)`**
4. **`err==RT_EOK`**：**`chan->data = (void *)data`**；若 **`timeout_ms != RT_WAITING_FOREVER`**，配置 **`chan->timer`** 为 **`rt_tick_from_millisecond(timeout_ms)`**，标记 **`timer_go`**
5. 否则 **`chan->complete = RT_TRUE`**
6. 解锁后若 **`timer_go`**：**`rt_timer_start`**

**注意**：**`send` 返回后** 若未超时路径，**`complete` 仍为假**，需控制器在发送完成中断里调 **`rt_mbox_send_done(RT_EOK)`**（或由 **`tx_done`** 侧视为完成）。

### 3.4 `rt_mbox_send_done`

关锁取 **`chan->data`** 并清空；解锁后调 **`client->tx_done(client, data, err)`**；最后 **`chan->complete = RT_TRUE`**（在锁外赋值，与 **`mbox_chan_timeout`** 竞态需由使用方保证语义）。

### 3.5 `mbox_chan_timeout`

若 **`!chan->complete`** 则 **`err = -RT_ETIMEOUT`**，否则 **`RT_EOK`**；统一 **`rt_mbox_send_done(chan, err)`**。

### 3.6 `rt_mbox_peek` / `rt_mbox_recv`

- **`peek`**：转 **`ops->peek`**（无则 **FALSE**）
- **`recv`**：若有 **`rx_callback`** 则 **`rx_callback(client, data)`**

### 3.7 `rt_mbox_request_by_index`

- 入口条件写作 **`if (!client && index < 0)`** 才返回错误；**`client == RT_NULL` 且 `index >= 0` 会继续执行**，存在 **空指针解引用风险**，调用方应保证 **`client` 非空**；更合理写法通常为 **`!client || index < 0`**。
- **`rt_ofw_parse_phandle_cells(np, "mboxes", "#mbox-cells", index, &args)`**
- 若 **`rt_ofw_data(ctrl_np)`** 为空：**`rt_platform_ofw_request(ctrl_np)`** 再取 **`ctrl`**
- **`index`**：优先 **`ops->ofw_parse(ctrl,&args)`**，否则 **`mbox_controller_ofw_parse_default`**（要求 **`args_count==1`**，返回 **`args[0]`**）
- 取 **`chan = &ctrl->chans[index]`**；若有 **`ops->request`** 则调用，失败则 **`rt_mbox_release`** 并返回错误指针
- **`chan->client = client`**

### 3.8 `rt_mbox_request_by_name`

**`rt_ofw_prop_index_of_string(np, "mbox-names", name)`** 得索引，再 **`rt_mbox_request_by_index`**；失败返回 **`RT_NULL`**（与 **`by_index`** 的 **`rt_err_ptr`** 风格不同）。

### 3.9 `rt_mbox_release`

直接 **`chan->ctrl->ops->release(chan)`**；**未** 判 **`ops->release`** 是否非空（控制器应始终提供有效 **`release`**，PIC 驱动已实现）。

---

## 4. PIC Mailbox 参考驱动（`mailbox-pic.c`）

### 4.1 寄存器布局（相对 **`regs`/`peer_regs` 基址**）

| 偏移 | 含义 |
|------|------|
| **0x00** | **IMASK**（通道中断屏蔽） |
| **0x04** | **ISTATE**（待处理位图） |
| **0x08 + n×4** | **MSG(n)**，每通道 **32 位** 消息 |

### 4.2 双端映射（**`position` DT 属性**）

- **`position == 0`**（“captain”）：**`regs = iomap(0)`**，**`peer_regs = regs + size/2`**；初始化两侧 **IMASK=0xffffffff**、**ISTATE=0**
- **`position != 0`**：**`peer_regs = iomap(0)`**，**`regs = peer_regs + size/2`**

即 **同一 `reg` 区域拆成两半**，分别视为 **本地 / 对端** 视图。

### 4.3 `pic_mbox_ops`

- **`request`**：清除本侧 **`IMASK`** 对应位，清 **ISTATE**
- **`release`**：置位 **IMASK**（屏蔽）
- **`send`**：自旋等待对端 **ISTATE** 对应位清零（**`rt_thread_yield`** 轮询）；若对端 **IMASK** 仍屏蔽该通道则 **`-RT_ERROR`**；否则写 **`MSG(index)`**，对端 **ISTATE** 置位，**`rt_hw_wmb()`**，**`rt_pic_irq_set_state_raw(pic, peer_hwirq, PENDING, TRUE)`** 触发对端中断

### 4.4 `pic_mbox_isr`

读 **ISTATE**，对每位读 **`peer_regs + MAILBOX_MSG(idx)`**（对端写入的载荷），**`rt_mbox_recv(&chans[idx], &msg)`**；最后清 **ISTATE** 位。

### 4.5 `probe` / `remove`

- **DT**：**`compatible = "rt-thread,pic-mailbox"`**；**`reg`、`position`、`interrupts`、`peer-interrupts`、`uid`**；**`#mbox-cells = <1>`**（与默认 **`ofw_parse`** 一致）
- **`chans_nr = 32`**，**`rt_mbox_controller_register`**
- **`rt_hw_interrupt_install` + umask`**

**`remove`**：从 **`pdev->parent.user_data`** 取 **`pic_mbox`**。**`probe` 路径未对 `user_data` 赋值**（控制器上下文通过 **`rt_dm_dev_bind_fwdata` → `rt_ofw_data(dev->ofw_node)`** 绑定），若平台层未额外写入 **`user_data`**，**`remove` 可能与 `probe` 不一致**，移植时需改为 **`rt_ofw_data(dev->ofw_node)` 容器转换** 或与 **`bind_fwdata`** 约定统一取指方式。

---

## 5. 设备树侧约定（与 Linux 对齐思路）

- 消费节点：**`mboxes = <&mailbox 0>;`**，可选 **`mbox-names = "vq", "foo";`**
- 控制器节点：**`#mbox-cells`** 与 **`ofw_parse`/`default`** 一致（默认 **1 个 cell = 通道号**）

---

## 6. 使用要点（驱动作者）

1. 填好 **`rt_mbox_client`**（**`dev`、`rx_callback`、`tx_*`**），**`rt_mbox_request_*`** 得到 **`chan`**。
2. **`rt_mbox_send`** 后必须在 **硬件完成** 或 **错误** 路径调用 **`rt_mbox_send_done`**，否则 **`complete`** 与 **`tx_done`** 行为不确定；需要超时则传 **`timeout_ms`**。
3. **`send` 的 `data` 指针** 在 **`send_done`** 之前需保持有效（框架只存指针）。
4. PIC 参考实现假定 **32 位消息**；其它 SoC 应自定义 **`ops->send`** 与 **`recv` 数据宽度**。

---

## 7. 小结

| 项目 | 说明 |
|------|------|
| 全局链表 | **`mbox_nodes` + `mbox_ops_lock`**，便于遍历或调试扩展 |
| 超时机制 | **每通道 `rt_timer` ONE_SHOT** |
| 参考硬件 | **`rt-thread,pic-mailbox`**：双半区 MMIO + **PIC** 投递对端 **IRQ** |
| 风险点 | **`rt_mbox_request_by_index` 入口条件**；**`pic_mbox_remove` 与 `user_data`** 建议核对 BSP |

阅读顺序：**`mailbox.h`** → **`mailbox.c`**（**`send`/`send_done`/请求路径**）→ **`mailbox-pic.c`**（寄存器与 **ISR**）。
