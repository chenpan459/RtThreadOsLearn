# RT-Thread 5.2.0 CAN 总线代码详细分析

本文面向源码阅读，围绕以下文件展开：

- 框架层：`rt-thread-5.2.0/components/drivers/can/dev_can.c`
- 对外接口：`rt-thread-5.2.0/components/drivers/include/drivers/dev_can.h`
- 参考说明：`rt-thread-5.2.0/components/drivers/can/readme-zh.txt`
- BSP 示例：`rt-thread-5.2.0/bsp/n32g452xx/Libraries/rt_drivers/drv_can.c`

---

## 1. 模块定位与分层

RT-Thread CAN 子系统采用“**通用框架 + 芯片驱动回调**”分层：

- **通用框架层（dev_can.c）**
  - 负责 `rt_device` 生命周期（init/open/close/read/write/control）
  - 负责中断模式下的发送/接收软件队列管理
  - 负责状态统计、回调通知、超时定时器
- **芯片驱动层（BSP drv_can.c）**
  - 实现 `struct rt_can_ops`（`configure/control/sendmsg/recvmsg`）
  - 把硬件中断事件转换为 `rt_hw_can_isr(can, event)`
  - 实现波特率、模式、过滤器、启动停止等寄存器操作

你可以把 `dev_can.c` 理解为“协议无关的 CAN 设备中间件”，把各 BSP 的 `drv_can.c` 理解为“硬件适配器”。

---

## 2. 关键数据结构

### 2.1 `struct rt_can_device`

位于 `dev_can.h`，核心字段：

- `parent`：继承 `struct rt_device`
- `ops`：芯片驱动回调表 `rt_can_ops`
- `config`：配置（波特率、工作模式、邮箱尺寸、私有发送模式等）
- `status`：运行统计（收发计数、错误计数、丢包计数）
- `timer`：周期定时器，用于状态轮询/回调上报
- `can_rx` / `can_tx`：框架分配的软件 RX/TX 队列控制块
- `lock`：设备级互斥锁（open/close 等流程互斥）

### 2.2 `struct rt_can_ops`

由 BSP 驱动实现：

- `configure(can, cfg)`：按配置初始化控制器
- `control(can, cmd, arg)`：处理控制命令（中断开关、波特率、模式、过滤器等）
- `sendmsg(can, buf, boxno)`：发一帧到指定发送邮箱
- `recvmsg(can, buf, boxno)`：从指定接收通道取一帧

### 2.3 报文结构 `struct rt_can_msg`

- 支持标准/扩展 ID、RTR、长度、私有字段 `priv`
- 支持 `hdr_index`（硬件过滤器命中的 bank 索引）
- 在 `RT_CAN_USING_CANFD` 下扩展到 64 字节负载并带 FD/BRS 字段

---

## 3. 初始化与注册流程

入口函数：`rt_hw_can_register()`

主要动作：

1. 填充 `rt_device` 基本属性：`type = RT_Device_Class_CAN`
2. 初始化默认状态、锁、软队列指针
3. 绑定设备操作（`rt_can_init/open/close/read/write/control`）
4. 初始化周期定时器 `cantimeout`
5. 调用 `rt_device_register()` 注册为字符设备

这一步完成后，CAN 设备进入系统设备表，应用可通过 `rt_device_find/open` 使用。

---

## 4. 打开/关闭设备时做了什么

### 4.1 `rt_can_open()`

核心逻辑：

- 若带 `RT_DEVICE_FLAG_INT_RX`：
  - 分配 `rt_can_rx_fifo` 与 `msgboxsz` 个消息节点
  - 建立 `freelist` / `uselist`
  - 下发 `RT_DEVICE_CTRL_SET_INT` 开启 RX 中断
- 若带 `RT_DEVICE_FLAG_INT_TX`：
  - 分配 `rt_can_tx_fifo` 与 `sndboxnumber` 个发送槽
  - 初始化发送信号量（容量等于发送邮箱数）
  - 下发 `RT_DEVICE_CTRL_SET_INT` 开启 TX 中断
- 开启错误中断 `RT_DEVICE_CAN_INT_ERR`
- 首次打开时启动状态定时器

### 4.2 `rt_can_close()`

- 引用计数大于 1 时不真正释放（多打开保护）
- 停止状态定时器
- 关闭 RX/TX/ERR 中断
- 释放 RX/TX 软件队列及信号量
- 下发 `RT_CAN_CMD_START` 停止硬件

---

## 5. 发送路径详解

用户调用 `rt_device_write()` 后进入 `rt_can_write()`：

- 若设备是中断发送模式：
  - `privmode = 0`：走 `_can_int_tx()`
  - `privmode = 1`：走 `_can_int_tx_priv()`

### 5.1 普通发送 `_can_int_tx()`

流程要点：

1. `tx_fifo->sem` 限流（确保不超过硬件发送槽数量）
2. 从 `freelist` 取一个发送槽，计算邮箱号 `no`
3. 调驱动 `ops->sendmsg(can, data, no)` 触发硬件发送
4. 阻塞等待该槽的 `completion`
5. 中断里收到 `TX_DONE`/`TX_FAIL` 后唤醒
6. 成功则计数 `sndpkg++`，失败计数 `dropedsndpkg++`

这是典型的“**异步硬件 + 同步等待完成**”封装。

### 5.2 私有发送 `_can_int_tx_priv()`

- 直接使用 `msg.priv` 作为目标发送邮箱号
- 常用于对发送邮箱优先级/顺序有硬约束的场景
- 若对应邮箱未空闲，等待其 completion 再重试

---

## 6. 接收路径详解

用户调用 `rt_device_read()` 后进入 `rt_can_read()`，中断模式下走 `_can_int_rx()`：

1. 从 `uselist`（已使用列表）取消息节点
2. 拷贝到用户缓冲区
3. 节点放回 `freelist`
4. 返回实际字节数

接收数据本身由中断上半部 `rt_hw_can_isr()` 填入软件队列。

---

## 7. 中断处理 `rt_hw_can_isr()`（核心）

`event` 低 8 位是事件类型，高位带通道号（box/fifo index）。

### 7.1 `RT_CAN_EVENT_RX_IND` / `RXOF_IND`

- `RXOF_IND` 先记接收溢出计数
- 读取硬件报文：`ops->recvmsg(can, &tmpmsg, no)`
- 从 `freelist` 取节点；若无空闲则覆盖 `uselist` 旧数据并计丢包
- 将新报文放入 `uselist`
- 若启用 HDR 过滤器并命中，对应过滤器链表计数并触发独立回调
- 否则触发设备级 `rx_indicate(dev, rx_length)`

### 7.2 `RT_CAN_EVENT_TX_DONE` / `TX_FAIL`

- 根据事件更新发送槽结果（OK/ERR）
- `rt_completion_done()` 唤醒写线程

结论：`rt_hw_can_isr()` 是“硬件事件 -> 软件队列/同步原语”的桥接点。

---

## 8. `control` 命令面板

`rt_can_control()` 在框架层处理部分通用命令，其他透传给 BSP：

- 设备电源状态：`RT_DEVICE_CTRL_SUSPEND/RESUME`
- 配置：`RT_DEVICE_CTRL_CONFIG`
- 私有发送模式：`RT_CAN_CMD_SET_PRIV`
- 状态回调注册：`RT_CAN_CMD_SET_STATUS_IND`
- 过滤器：`RT_CAN_CMD_SET_FILTER`（含 HDR 路径下的软件镜像管理）
- 其余命令透传 `ops->control`

常用 CAN 命令宏在 `dev_can.h`：

- `RT_CAN_CMD_SET_BAUD`
- `RT_CAN_CMD_SET_MODE`
- `RT_CAN_CMD_GET_STATUS`
- `RT_CAN_CMD_START`
- CANFD 相关：`SET_CANFD` / `SET_BAUD_FD` / `SET_BITTIMING`

---

## 9. 定时器与状态上报

`cantimeout()` 每个周期执行：

1. `RT_CAN_CMD_GET_STATUS` 拉取硬件状态到 `can->status`
2. 若用户注册 `status_indicate`，回调通知
3. 若启用 `RT_CAN_USING_BUS_HOOK`，调用总线钩子

它是运行时健康监测和状态订阅机制，不负责实时收发。

---

## 10. BSP 适配示例（n32g452xx）

参考 `bsp/n32g452xx/Libraries/rt_drivers/drv_can.c` 可以看到标准落地方式：

- 用芯片时钟参数建立波特率表（`prescaler/tsjw/tbs1/tbs2`）
- `_can_config()` 把 RT 配置映射到硬件寄存器
- `_can_control()` 处理中断使能、模式切换、过滤器配置等命令
- 中断服务里根据硬件状态组织事件码并调用 `rt_hw_can_isr()`

这说明：

- **框架层不感知具体芯片寄存器**
- **芯片驱动不重复实现队列/同步/设备框架**

二者职责边界清晰。

---

## 11. 代码设计特点与注意点

### 优点

- 统一 `rt_device` 接口，应用层切换 CAN 控制器代价低
- 中断模式下用软件 FIFO + completion，语义清晰
- 兼容 HDR 过滤、私有发送模式、CANFD 扩展

### 注意点

- 当前主路径是中断模式，轮询模式支持较弱
- `msgboxsz` 太小会导致 `dropedrcvpkg` 增长
- 私有发送模式依赖 `priv` 字段合法性（小于 `sndboxnumber`）
- 过滤器 HDR 路径下要注意 `hdr_bank` 与 `maxhdr` 对齐

---

## 12. 应用侧最小使用流程

1. `rt_device_find("canX")`
2. `rt_device_open(can, RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX)`
3. （可选）`rt_device_control(can, RT_CAN_CMD_SET_FILTER, ...)`
4. `rt_device_set_rx_indicate(can, rx_cb)` + 线程/信号量取数
5. `rt_device_write` 发送 `struct rt_can_msg`
6. `rt_device_read` 接收 `struct rt_can_msg`
7. `rt_device_control(can, RT_CAN_CMD_GET_STATUS, ...)` 查询状态

---

## 13. 在当前 QEMU vexpress-a9 BSP 的现实情况

你当前工作目录是 `bsp/qemu-vexpress-a9`。该 BSP 默认重点是串口、网卡等，通常不带可用 CAN 控制器驱动。  
因此本分析主要针对 RT-Thread CAN 框架通用实现；若要实测 CAN，建议切到具备 CAN 外设并已提供 `drv_can.c` 的 MCU BSP（如上面示例类 BSP）。

---

## 14. CAN 总线协议描述（协议层）

这部分补充“CAN 协议本身”的核心规则，便于把驱动代码和总线行为对应起来。

### 14.1 CAN 2.0 帧类型与字段

CAN 2.0 常见两类帧：

- **数据帧（Data Frame）**：承载实际应用数据
- **远程帧（Remote Frame）**：请求对端发送指定 ID 的数据

主要字段（逻辑顺序）：

1. **SOF**：起始位（显性）
2. **仲裁域**：ID + RTR（决定总线仲裁优先级）
3. **控制域**：IDE/DLC 等
4. **数据域**：0~8 字节（经典 CAN）
5. **CRC 域**：循环冗余校验
6. **ACK 域**：接收节点应答
7. **EOF/IFS**：结束与帧间隔

与 RT-Thread `rt_can_msg` 的映射：

- `id`：仲裁 ID（11 位标准 / 29 位扩展）
- `ide`：标准帧(`RT_CAN_STDID`)或扩展帧(`RT_CAN_EXTID`)
- `rtr`：数据帧(`RT_CAN_DTR`)或远程帧(`RT_CAN_RTR`)
- `len`：DLC（经典 CAN 常用 0~8）
- `data[]`：有效载荷

### 14.2 帧格式（标准帧/扩展帧/远程帧/CAN FD）

下面给出常用帧的字段顺序（从左到右），并标注位宽（bit）：

1. **标准数据帧（11-bit ID）**
   - `SOF | ID[10:0] | RTR=0 | IDE=0 | r0 | DLC | DATA(0~8B) | CRC | CRC_DELIM | ACK | ACK_DELIM | EOF | IFS`
   - 位宽拆分：`1 + 11 + 1 + 1 + 1 + 4 + (0~64) + 15 + 1 + 1 + 1 + 7 + 3`
   - 不含数据位时开销：`47 bit`（约 `5.88 Byte`）
   - 满 8 字节数据时：`111 bit`（约 `13.88 Byte`）
2. **标准远程帧（11-bit ID）**
   - `SOF | ID[10:0] | RTR=1 | IDE=0 | r0 | DLC | (无DATA) | CRC | CRC_DELIM | ACK | ACK_DELIM | EOF | IFS`
   - 位宽拆分：`1 + 11 + 1 + 1 + 1 + 4 + 0 + 15 + 1 + 1 + 1 + 7 + 3`
   - 总长度：`47 bit`（约 `5.88 Byte`）
3. **扩展数据帧（29-bit ID）**
   - `SOF | ID_A[10:0] | SRR=1 | IDE=1 | ID_B[17:0] | RTR=0 | r1 | r0 | DLC | DATA | CRC | ACK | EOF | IFS`
   - 位宽拆分：`1 + 11 + 1 + 1 + 18 + 1 + 1 + 1 + 4 + (0~64) + 15 + 1 + 1 + 1 + 7 + 3`
   - 不含数据位时开销：`67 bit`（约 `8.38 Byte`）
   - 满 8 字节数据时：`131 bit`（约 `16.38 Byte`）
4. **扩展远程帧（29-bit ID）**
   - `SOF | ID_A[10:0] | SRR=1 | IDE=1 | ID_B[17:0] | RTR=1 | r1 | r0 | DLC | (无DATA) | CRC | ACK | EOF | IFS`
   - 位宽拆分：`1 + 11 + 1 + 1 + 18 + 1 + 1 + 1 + 4 + 0 + 15 + 1 + 1 + 1 + 7 + 3`
   - 总长度：`67 bit`（约 `8.38 Byte`）

补充说明：

- **SRR**：替代标准帧 RTR 位置的隐性位，保证标准帧在相同前缀下优先级更高。
- **DLC**：经典 CAN 中有效值常为 0~8；远程帧中 DLC 表示请求长度期望值。
- **ACK**：发送节点在 ACK 槽发送隐性位，任何正确接收节点会拉为显性位确认。
- 上述统计按“未计入位填充(bit stuffing)”计算；实际线上位数会因填充略增加。

#### 字段位宽速查（经典 CAN）

- `SOF`：1 bit
- `ID(标准)`：11 bit
- `ID(扩展)`：29 bit（拆分为 `ID_A 11 + ID_B 18`）
- `RTR/SRR/IDE/r0/r1`：各 1 bit
- `DLC`：4 bit
- `DATA`：0~64 bit（0~8 Byte）
- `CRC`：15 bit
- `CRC_DELIM`：1 bit
- `ACK`：2 bit（ACK 槽 1 bit + ACK 分隔符 1 bit）
- `EOF`：7 bit
- `IFS`：3 bit

#### CAN FD 帧格式要点

CAN FD 在仲裁阶段保持与经典 CAN 兼容，但控制域扩展：

- 关键位：`FDF`（FD 格式）、`BRS`（位速率切换）、`ESI`（错误状态指示）
- 数据长度：0~64 字节（由 DLC 编码映射）
- CRC：更长（17/21 位，取决于数据长度）

简化顺序可记为：

- `SOF | 仲裁域(ID/IDE/RTR) | 控制域(FDF/BRS/ESI/DLC) | DATA(0~64B) | CRC | ACK | EOF | IFS`

### 14.3 仲裁机制（CSMA/CR）

CAN 采用“**有线与**”电平规则：

- 显性位（0）覆盖隐性位（1）
- 多节点同时发时，逐位比较仲裁域
- 发送到隐性却读到显性的一方立即退出仲裁
- ID 越小优先级越高（更容易赢得总线）

这解释了为什么驱动里会有多发送邮箱与“私有发送模式”——应用侧可通过邮箱/ID 设计影响实时性，但最终仍遵循总线仲裁。

### 14.4 位填充（Bit Stuffing）

规则：在仲裁到 CRC 序列范围内，连续 5 个同极性位后需插入 1 个反相填充位。  
接收端自动去填充；若填充规则违背会计入位错误。

这对应 `rt_can_status` 中的错误统计项（如位错误、格式错误等），常用于诊断线缆干扰、时序不匹配或采样点配置异常。

### 14.5 错误检测与错误状态机

CAN 协议具备多层错误检测：

- 位错误（Bit Error）
- 填充错误（Stuff Error）
- CRC 错误
- 格式错误（Form Error）
- ACK 错误

节点维护发送/接收错误计数器（TEC/REC），并在三种状态间演进：

- **Error Active（正常）**
- **Error Passive（被动）**
- **Bus-Off（离线）**

RT-Thread 在 `rt_can_status` 中提供了这些统计和状态码，`canstat` 命令就是围绕它们做可视化输出。

### 14.6 过滤器机制（硬件/软件协同）

协议层面每帧都带 ID，控制器通常提供硬件过滤器（ID/Mask）减少 CPU 负担。  
RT-Thread 中：

- 硬件配置通过 `RT_CAN_CMD_SET_FILTER`
- 命中结果可映射到 `hdr_index`
- 框架可把消息分发到对应 HDR 链表和回调

即：**硬件先粗过滤，框架再按 bank 做软件分流**。

### 14.7 CAN FD 与经典 CAN 差异

CAN FD 核心变化：

- 数据域可达 64 字节（经典 CAN 为 8 字节）
- 仲裁阶段与数据阶段可用不同速率（BRS）
- 帧格式扩展（FDF/ESI/BRS 等相关位）

在 RT-Thread 中表现为：

- `RT_CAN_USING_CANFD` 打开后，`rt_can_msg.data` 扩到 64 字节
- 可通过 `RT_CAN_CMD_SET_CANFD`、`RT_CAN_CMD_SET_BAUD_FD`、`RT_CAN_CMD_SET_BITTIMING` 配置

### 14.8 协议与驱动代码的对应关系

- 协议“发完是否成功” -> `TX_DONE / TX_FAIL` 事件 -> completion 唤醒写线程
- 协议“接收帧到来” -> `RX_IND` 事件 -> 入 `uselist` 并回调 `rx_indicate`
- 协议“溢出/错误累积” -> `dropedrcvpkg` 与各类 error counter
- 协议“总线状态变化” -> `RT_CAN_CMD_GET_STATUS` 周期采样 + `status_indicate`

读代码时建议始终把“总线规则”与“框架状态变量”一一对应，定位问题会非常快。

---

## 15. 一句话总结

RT-Thread CAN 子系统的核心是：  
**用 `dev_can.c` 统一收发队列、同步与设备语义，用 BSP 的 `rt_can_ops` 承接硬件细节，再通过 `rt_hw_can_isr()` 将中断事件稳定汇入框架。**

