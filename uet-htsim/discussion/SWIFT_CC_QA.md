# Swift CC Q&A（逐条讨论记录）

状态说明：
- `已讨论`：本轮已经达成共识。
- `待讨论`：下一轮继续。

---

## Q1（已讨论）：`_rtt` 是如何维护的？

### 1) `_rtt` 的归属与初始化
- `_rtt` 是 **每个 SwiftSubflowSrc 的状态**，不是全局状态。
- 定义在 `SwiftSubflowSrc`：
  - `htsim/sim/swift.h:72`
- 构造时初始化为 0：
  - `htsim/sim/swift.cpp:23`

相关初值（同一组）：
- `_rto = 1ms`：`htsim/sim/swift.cpp:24`
- `_min_rto = 100us`：`htsim/sim/swift.cpp:25`
- `_mdev = 0`：`htsim/sim/swift.cpp:26`

### 2) `_rtt` 何时更新
- 在收到 ACK 后，先计算本次样本时延 `delay = now - ts_echo`：
  - `htsim/sim/swift.cpp:459`
- 然后调用 `adjust_cwnd(delay, ackno)`：
  - `htsim/sim/swift.cpp:460`
- `adjust_cwnd` 里会调用 `update_rtt(delay)`：
  - `htsim/sim/swift.cpp:86`

也就是说：**`_rtt` 是 ACK 驱动更新的平滑 RTT 估计**。

### 3) 更新公式（`update_rtt`）
实现位置：
- `htsim/sim/swift.cpp:52`

逻辑：
1. 若 `delay != 0` 才更新。  
2. 若已有历史 RTT（`_rtt > 0`）：
   - `abs = |delay - _rtt|`
   - `_mdev = 3/4 * _mdev + 1/4 * abs`
   - `_rtt  = 7/8 * _rtt  + 1/8 * delay`
   - `_rto  = _rtt + 4 * _mdev`
3. 若是第一次样本（`_rtt == 0`）：
   - `_rtt = delay`
   - `_mdev = delay / 2`
   - `_rto = _rtt + 4 * _mdev`
4. 最后做下限保护：
   - `if (_rto < _min_rto) _rto = _min_rto`
   - `htsim/sim/swift.cpp:72`

这是典型的 TCP 风格平滑 RTT/RTO 估计（Jacobson/Karels 族）。

### 4) `_rtt` 在 Swift 控制律中的用途
`_rtt` 不只是观测值，它直接参与控制：

1. 约束降窗频率（每 RTT 至多一次）  
- `_can_decrease = (now - _last_decrease) >= _rtt`
- `htsim/sim/swift.cpp:82`

2. pacing 计算  
- 当 `cwnd < mss` 时：
  - `_pacing_delay = (_rtt * mss()) / _swift_cwnd`
  - `htsim/sim/swift.cpp:165`

3. PLB 相关定时阈值  
- `_plb_interval` 里使用 `_rtt`：
  - `htsim/sim/swift.cpp:115`

4. 超时日志/诊断  
- 打印 RTO/MDEV/RTT：
  - `htsim/sim/swift.cpp:476`

### 5) 结论（Q1）
- `_rtt` 是 per-subflow 的平滑 RTT 估计。
- 由 ACK 的 timestamp echo 驱动更新。
- 与 `_mdev/_rto` 一起形成超时估计，并且直接影响：
  - MD 触发节奏（降窗门控）
  - pacing 间隔
  - PLB 时间尺度

---

## Q2（已讨论）：`_mdev` 是什么？命名含义是什么？

### 1) `_mdev` 的定义
- `_mdev` 是 RTT 样本波动的平滑估计，语义接近 **mean deviation / mean absolute deviation**（平均偏差的 EWMA 版本）。
- 定义位置：
  - `htsim/sim/swift.h:72`
- 初始化：
  - `htsim/sim/swift.cpp:26`（初始为 0）

### 2) 名字来源（mdev）
- `mdev` 这个命名来自 TCP RTO 估计家族里的传统写法，通常表示 **RTT 偏差项**。
- 在这份代码里它不是“方差”也不是“标准差”，而是对 `|sampleRTT - srtt|` 的指数平滑。

### 3) 代码里的更新方式
更新函数：
- `htsim/sim/swift.cpp:52`（`update_rtt`）

当已有 RTT 估计（`_rtt > 0`）时：
- `abs = |delay - _rtt|`
- `_mdev = 3/4 * _mdev + 1/4 * abs`  （`htsim/sim/swift.cpp:62`）

第一次样本时（`_rtt == 0`）：
- `_mdev = delay / 2`（`htsim/sim/swift.cpp:67`）

### 4) `_mdev` 的作用
它直接进入 RTO 计算：
- `_rto = _rtt + 4 * _mdev`（`htsim/sim/swift.cpp:64`、`htsim/sim/swift.cpp:68`）
- 再做下限保护：
  - `if (_rto < _min_rto) _rto = _min_rto`（`htsim/sim/swift.cpp:72`）

直观含义：
- 网络抖动变大 -> `_mdev` 变大 -> RTO 变长 -> 超时判断更保守（减少误超时）
- 网络更稳定 -> `_mdev` 变小 -> RTO 回落 -> 超时更敏捷

### 5) 和 CC 的关系边界
- `_mdev` 本身不直接参与“加窗/减窗”公式（AI/MD 由 delay-vs-target 决定）。
- 但它通过 RTO 间接影响拥塞行为：  
  超时事件频率变化 -> 触发 `on timeout` 的回退路径变化。

### 6) 结论（Q2）
- `_mdev` 可以理解成“RTT 抖动强度的平滑指标”。
- 命名含义是 mean deviation（偏差项），用于构造 `RTO = RTT + 4*MDEV`。
- 在 Swift 里它主要服务于超时鲁棒性，而不是直接参与 delay-based AIMD 主环。

## Q3（已讨论）：`num_acked`、`ackno`、`_last_acked` 各代表什么？

### 1) `ackno` 是什么
- `ackno` 来自 ACK 包字段 `SwiftAck::_ackno`：
  - 定义：`htsim/sim/swiftpacket.h:81`
  - 读取：`ackno()`，`htsim/sim/swiftpacket.h:72`
- 发送 ACK 时，sink 用 `_cumulative_ack` 填入该字段：
  - `htsim/sim/swift.cpp:957`

语义：
- `ackno` 表示“**当前累计确认到的子流字节序号（最后一个已连续确认字节）**”。

补充：
- Swift 里 `seqno` 是包首字节，但底层路由里用了 `seqno+size-1` 作为 packet id：
  - `htsim/sim/swiftpacket.h:22`
- 这会让阅读上有点绕，但 `ackno` 本质仍是累计 ACK 点。

### 2) `_last_acked` 是什么
- `_last_acked` 是 sender 侧记录的“上一轮已处理的累计 ACK 点”。
- 初始化为 0：
  - `htsim/sim/swift.cpp:21`
- 在 `handle_ack()` 里，凡是“新 ACK”（`ackno > _last_acked`）路径都会推进它：
  - 非 FR：`htsim/sim/swift.cpp:188`
  - FR 结束：`htsim/sim/swift.cpp:206`
  - FR 中间：`htsim/sim/swift.cpp:230`

### 3) `num_acked = ackno - _last_acked` 是什么
- 计算位置：
  - `htsim/sim/swift.cpp:94`
- 它是在 `adjust_cwnd()` 里用于 AI 增窗的“本次 ACK 相比上一 ACK 推进了多少字节”的估计量。

关键细节：
- 在调用顺序上，`adjust_cwnd(delay, ackno)` 先于 `handle_ack(ackno)`：
  - `htsim/sim/swift.cpp:460`
  - `htsim/sim/swift.cpp:464`
- 所以计算 `num_acked` 时，`_last_acked` 还是“上一次”的值，这是故意的。

### 4) 它是否等价于“真正新确认字节数”？
- 在理想累计 ACK 情况下，基本等价。
- 但代码也承认边界情况：
  - 若 `ackno - _last_acked < 0`，会被截断为 0（防御性处理）：
    - `htsim/sim/swift.cpp:96`
- 另外在 fast recovery 中，`ackno` 推进和有效交付关系可能更复杂，代码用 `_inflate` 等额外状态补偿：
  - `htsim/sim/swift.cpp:224` 之后。

所以更准确说法：
- `num_acked` 是 **用于 CC 更新的 ACK 推进量**，不是严格意义上的“应用层新交付字节统计”。

### 5) 和 `ds_ackno` 的区别（容易混淆）
- `ackno`：子流序号空间（subflow seq）上的累计 ACK。
- `ds_ackno`：数据序号空间（DSN）上的累计 ACK，用于连接级完成判断：
  - 接收：`htsim/sim/swift.cpp:436`
  - 上报：`htsim/sim/swift.cpp:462`

### 6) 结论（Q3）
- `ackno`：当前累计确认点（子流字节序号）。
- `_last_acked`：sender 已处理的上一个累计确认点。
- `num_acked=ackno-_last_acked`：本轮 ACK 推进量，用于 AI 增窗；是 CC 近似量，不是严格业务交付统计量。

## Q4（已讨论）：`mss` 是什么？和 Sing 里的 `mtu` 的关系？

### 1) 在 Swift 代码里，`mss` 的实际含义
- Swift 里 `mss()` 返回 `SwiftSrc::_mss`：
  - `htsim/sim/swift.h:180`
- `_mss` 在构造时设置为 `Packet::data_packet_size()`：
  - `htsim/sim/swift.cpp:595`

这意味着 Swift 里的 `mss` 是“**每个数据包的有效发送粒度（字节）**”，并且它直接就是框架当前数据包大小。

### 2) 在 Sing 代码里，`mtu/mss` 是显式区分的
- `SingSrc` 明确区分：
  - `_mtu`：含头（`htsim/sim/sing.h:254`）
  - `_mss`：不含头（`htsim/sim/sing.h:253`）
- 运行时赋值：
  - `_mtu = Packet::data_packet_size()`
  - `_mss = _mtu - _hdr_size`
  - `htsim/sim/sing.cpp:656`

并且 `main_sing` 会根据 `-mtu` 设置 `Packet::set_packet_size(packet_size)`：
- `htsim/sim/datacenter/main_sing.cpp:600`

### 3) 两边的对应关系（重点）
- Swift 的 `mss`（当前实现）≈ Sing 的 `mtu`（数据包总大小）。
- Sing 的 `_mss` 是 payload-only，通常小于 `mtu`。

所以不能简单把名字等同：
- Swift `mss` 名字像 TCP MSS，但在这份实现里更接近“packet size quantum”。

### 4) 对迁移到 Sing CC 的影响
- 迁移 Swift 控制律到 `BaseCC` 时，所有 `mss()` 出现处建议映射到 **Sing 的 `_mtu`**（或显式命名 `pkt_bytes`），而不是 `Sing::_mss`。
- 否则 AI/FR 的步长会被系统性缩小，行为偏离原 Swift。

### 5) 结论（Q4）
- Swift 代码中的 `mss` 不是你在 Sing 里看到的 payload-only `_mss`。
- 更准确地说，它等价于“每个数据包的发送粒度”，在当前工程里与 `Packet::data_packet_size()`一致，因此更接近 Sing 的 `_mtu` 语义。

## Q5（已讨论）：`targetDelay` 的计算 rationale 是什么？

### 1) `targetDelay` 的公式是什么
实现位置：
- `htsim/sim/swift.cpp:775`

公式可写为：
- `target_delay = base_delay + fs_delay(cwnd) + hop_delay(route)`

具体项：
1. `base_delay = _base_delay`（默认 20us，`htsim/sim/swift.cpp:608`）
2. `hop_delay = route.hop_count() * _h`（`htsim/sim/swift.cpp:794`）
3. `fs_delay(cwnd) = _fs_alpha / sqrt(cwnd/_mss) + _fs_beta`，并 clamp 到 `[0, _fs_range]`（`htsim/sim/swift.cpp:777` 之后）

### 2) 每一项的直觉动机（rationale）
1. `base_delay`：
- 给所有流一个统一基础排队目标，作为“系统最低目标延迟预算”。

2. `hop_delay`：
- 路径 hop 越多，传播/转发时延天然更高；目标应对长路径更宽容。
- 否则长路径流会被系统性误判为“超目标”而过度减窗。

3. `fs_delay(cwnd)`（flow-scaling）：
- 当 `cwnd` 小时，`1/sqrt(cwnd)` 大，`fs_delay` 更高，目标更宽松。
- 当 `cwnd` 大时，`fs_delay` 下降并趋近 0，目标变紧。
- 这在工程上是在做“短小流/小窗更宽容，大窗流更严格”的延迟预算分层。

### 3) 为什么是 `1/sqrt(cwnd)` 形状
- 它是“递减但边际递减”的曲线：
  - 小 cwnd 区间变化敏感（更快给短流宽容）
  - 大 cwnd 区间变化平缓（避免目标过快塌缩）
- 相比线性/反比，`1/sqrt` 更不激进，稳定性更友好。

### 4) 参数初始化如何配合这个形状
在 `SwiftSrc` 构造里：
- `_fs_range = 5 * _base_delay`
- `_fs_min_cwnd = 0.1`
- `_fs_max_cwnd = 100`
- `_fs_alpha`、`_fs_beta` 按区间端点反解（`htsim/sim/swift.cpp:617` 起）

目的：
- 保证 `fs_delay` 在预期 cwnd 区间内有可控范围，并通过 clamp 防止极端值。

### 5) 它在控制环里的作用
`adjust_cwnd()` 用 `delay` 与 `target_delay` 做比较（`htsim/sim/swift.cpp:90`）：
- `delay < target`：加窗（AI）
- `delay >= target` 且允许：减窗（MD）

所以 `targetDelay` 本质是 **delay-based AIMD 的“判决阈值函数”**。

### 6) 一个关键细节：`targetDelay(0, route)` 的用途
- 在 PLB 逻辑中会调用 `targetDelay(0, route)`（`htsim/sim/swift.cpp:110`）。
- 因为 `cwnd==0` 时代码把 `fs_delay` 置 0，这样得到的是“不含流规模项的纯路径阈值”。
- 这相当于把路径健康判断和流规模影响分离。

### 7) 结论（Q5）
- `targetDelay` 不是单常数，而是“基础预算 + 路径补偿 + 流规模补偿”的组合阈值。
- 它的核心目的：让同一套 delay-based CC 在不同路径长度、不同流规模下有更一致的控制语义，减少系统性偏置。

### 8) 子问题补充：`hop_delay` 具体怎么算？`_h` 初值是什么？
1. `hop_delay` 的计算
- 在 `targetDelay()` 里直接计算：
  - `hop_delay = route.hop_count() * _h`
  - 代码：`htsim/sim/swift.cpp:792`

2. `_h` 的初值
- 在 `SwiftSrc` 构造函数中初始化为：
  - `_h = _base_delay / 6.55`
  - 代码：`htsim/sim/swift.cpp:609`
- 同时 `_base_delay` 默认是 `20us`（`htsim/sim/swift.cpp:608`），因此默认
  - `_h ≈ 20us / 6.55 ≈ 3.05us/ hop`

3. `_h` 可调方式
- 可通过 `set_hdiv(hdiv)` 改写：
  - `_h = _base_delay / hdiv`
  - 代码：`htsim/sim/swift.cpp:677`

4. 含义
- `_h` 是“每跳额外目标时延预算”的比例因子。
- 跳数越多，`targetDelay` 线性上升，避免长路径流被系统性惩罚。

## Q6（已讨论）：`handle_ack` 主要做什么？和 CC 的关系边界是什么？

### 1) `handle_ack` 的主职责（先结论）
- `handle_ack` 本质是 **ACK 驱动的可靠传输恢复状态机**，核心处理：
  - 新 ACK / 重复 ACK 分类
  - dupack 计数
  - fast recovery 进入/维持/退出
  - fast retransmit 触发
  - RTO 计时器重置
  - 发送窗口膨胀量 `_inflate` 管理

入口函数：
- `htsim/sim/swift.cpp:173`

### 2) 它具体干了哪些事
1. 新 ACK（`ackno > _last_acked`）路径  
- 刷新 RTO 超时点（`_RFC2988_RTO_timeout = now + _rto`）
- 更新 `_highest_sent` / 关闭计时器条件
- 若不在 FR：推进 `_last_acked`，清 `_dupacks`，`applySwiftLimits()`，`send_packets()`
- 若在 FR：
  - `ackno >= _recoverq`：退出 FR
  - `ackno < _recoverq`：按部分恢复路径继续 retransmit

关键位置：
- `htsim/sim/swift.cpp:175`

2. 重复 ACK（`ackno <= _last_acked`）路径  
- 在 FR 内：继续膨胀 `_inflate` 并尝试发包
- 不在 FR：`_dupacks++`
  - 未到 3：普通 dupack 处理
  - 到 3：触发快重传并进入 FR（受 `_recoverq` 门控）

关键位置：
- `htsim/sim/swift.cpp:238`

### 3) 和 CC 的关系：哪些是“纯可靠性”，哪些是“CC耦合”
`handle_ack` 不是纯可靠性代码，它有明显 CC 写操作。

A. 偏“可靠性/恢复机制”的动作：
- `_dupacks` 计数
- `_recoverq` 管理
- `_in_fast_recovery` 状态切换
- `retransmit_packet()` 调用
- RTO 计时器刷新

B. 直接影响 CC 的动作：
1. `_inflate` 影响可发送窗口  
- 发送判断用的是 `cwnd + inflate`（见 `send_packets()`）
- 所以 FR 直接改变发送速率节奏

2. FR 退出时的额外 MD  
- `if (_can_decrease) _swift_cwnd *= (1 - _max_mdf);`
- 这是明确的 cwnd 改写

3. 调用 `applySwiftLimits()`  
- 会做 cwnd 边界裁剪，并可能更新 pacing 参数

对应位置：
- `_inflate` 更新：`htsim/sim/swift.cpp:231`、`htsim/sim/swift.cpp:240`
- FR 退出额外 MD：`htsim/sim/swift.cpp:212`
- `applySwiftLimits()` 调用：`htsim/sim/swift.cpp:191`、`216`、`234`、`255`、`271`

### 4) 与 `adjust_cwnd()` 的分工关系
- `adjust_cwnd()`：delay-based AIMD 主环（每 ACK 做 delay 判决）
- `handle_ack()`：ACK恢复状态机 + loss/dupack 语义 + 局部窗口修正

调用顺序是：
- 先 `adjust_cwnd(delay, ackno)`
- 后 `handle_ack(ackno)`
- 见 `htsim/sim/swift.cpp:460`、`464`

这意味着：
- 先做“delay反馈控制”
- 再做“可靠性恢复语义修正”

### 5) 迁移到 Sing `BaseCC` 时的边界建议
如果 Phase C 只迁移 delay-based 控制律：
- `handle_ack` 中这些应 **不迁移** 到 `BaseCC`：
  - `_dupacks/_recoverq/_in_fast_recovery/_inflate/retransmit_packet`
  - RTO timer 状态机
- 可迁移或重解释的只有：
  - FR 退出时那类“loss触发额外MD”的策略（若要保真再考虑）

原因：
- 这些机制在 Sing 已有自己的 ACK/NACK/SACK/timeout 主链路，不宜再叠一套 Swift 传输态机。

### 6) 结论（Q6）
- `handle_ack` 主要职责是可靠性恢复状态机，不是纯 CC 函数。
- 但它和 CC 强耦合：通过 `_inflate` 与 FR 退出 MD 直接影响发送窗口。
- 对 Sing 的“仅迁移 delay-based 控制律”路线，应把 `handle_ack` 视为“整体不迁移、只提炼可选策略点”的模块。

## Q7（已讨论）：`applySwiftLimits` 里的 `pacing_delay` 在哪里使用？作用是什么？

### 1) `pacing_delay` 在哪里算出来
- 计算位置在 `applySwiftLimits()`：
  - `htsim/sim/swift.cpp:152`
- 规则：
  - 若 `cwnd < mss`：`_pacing_delay = (_rtt * mss()) / _swift_cwnd`（`htsim/sim/swift.cpp:165`）
  - 否则：`_pacing_delay = 0`（`htsim/sim/swift.cpp:168`）

直觉：
- 当窗口小于 1 个包时，不能按“窗口模式”一次发满，只能转为“按时间间隔发单包”。

### 2) 它在哪里被使用（调用链）
完整链路如下：

1. ACK 处理后会调用 `applySwiftLimits()`（例如 `handle_ack` 路径）  
  - 见 `htsim/sim/swift.cpp:191`、`216`、`234`、`255`、`271`

2. 随后进入 `send_packets()`，先计算 `c = cwnd + inflate`  
  - `htsim/sim/swift.cpp:283`

3. 若 `c < mss`，进入 pacing 模式  
  - `htsim/sim/swift.cpp:300`
  - 若 pacer 尚未 pending，则 `schedule_send(_pacing_delay)`：
    - `htsim/sim/swift.cpp:313`

4. `SwiftPacer::doNextEvent()` 到点触发后发送 1 个包：  
  - `_sub->send_next_packet()`：`htsim/sim/swift.cpp:861`
  - 然后根据最新 `_sub->pacing_delay()` 再次调度下一次：
    - `htsim/sim/swift.cpp:865`

5. 当窗口恢复（可窗口发送）时，`send_packets()` 会取消 pacer：  
  - `if (_pacer.is_pending()) _pacer.cancel();`
  - `htsim/sim/swift.cpp:337`

### 3) `SwiftPacer` 的角色
- `SwiftPacer` 是一个 EventSource 定时器对象（`htsim/sim/swift.h:28`）
- 维护：
  - `_last_send`
  - `_next_send`
  - `_interpacket_delay`
- `schedule_send()` 负责安排下一次发送时刻（`htsim/sim/swift.cpp:829`）
- `just_sent()` 在每次发包后更新 `_last_send`（`htsim/sim/swift.cpp:854`，调用点 `swift.cpp:387`）

### 4) 作用是什么（系统意义）
1. 保证 `cwnd < 1pkt` 时仍能平滑发送，而不是“全停/突发”
2. 将窗口控制从“包数域”转到“时间域”实现，避免子包窗口下的抖动
3. 与调度器协同：`send_next_packet()` 仍要过队列/调度器门控（`htsim/sim/swift.cpp:361`）

### 5) 和迁移到 Sing 的关系
- 你当前路线是“只迁移 delay-based cwnd 控制律”，不迁移 Swift 发送栈。
- 那么 `pacing_delay` 这套机制默认不会自动带过去，因为它依赖：
  - `SwiftPacer`
  - `send_packets()/send_next_packet()` 生命周期
- 所以 Phase C 若不扩 `BaseCC` 的 pacing 接口，可先不实现这一段（此前我们也讨论过这一点）。

### 6) 结论（Q7）
- `pacing_delay` 是 Swift 在“窗口不足 1 包”场景下的时间域发包间隔。
- 它通过 `SwiftPacer` 周期调度单包发送，并在窗口恢复后退出 pacing 模式。
- 这是发送机制层能力，不是纯粹的 cwnd 更新公式；迁移到 Sing 时需要单独接口支持才可保真复用。

## Q8（已讨论，新增）：Swift 里的 Fast Recovery 怎么做？何时触发？触发后影响什么？

### 1) 相关状态
- `_in_fast_recovery`：是否在 FR 中（`htsim/sim/swift.h:77`）
- `_dupacks`：重复 ACK 计数（`htsim/sim/swift.h:85`）
- `_recoverq`：恢复边界（进入 FR 时记下的 `_highest_sent`，`htsim/sim/swift.cpp:274`）
- `_inflate`：FR 期间临时窗口膨胀量（`htsim/sim/swift.h:75`）

### 2) 触发条件（进入 FR）
入口在 `handle_ack()`：`htsim/sim/swift.cpp:173`

触发序列：
1. 当前 ACK 不是新 ACK（`ackno <= _last_acked`），进入 dupack 分支（`htsim/sim/swift.cpp:238`）
2. 若还没在 FR，`_dupacks++`（`htsim/sim/swift.cpp:251`）
3. 当 `_dupacks == 3`，且 `!(_last_acked < _recoverq)`（避免未恢复时重复进入）才真正进入 FR（`htsim/sim/swift.cpp:259`）

进入 FR 时动作（`htsim/sim/swift.cpp:267` 起）：
- `_drops++`
- `retransmit_packet()` 立即快重传
- `_in_fast_recovery = true`
- `_recoverq = _highest_sent`
- 记录日志事件 `SWIFT_RCV_DUP_FASTXMIT`

### 3) FR 期间怎么运行
1. 若收到“仍是 dupack”的 ACK（`ackno <= _last_acked` 且 `_in_fast_recovery`）：
- `_inflate += mss()`，并限制不超过 `_swift_cwnd`
- 尝试继续发包 `send_packets()`
- 见 `htsim/sim/swift.cpp:239`

2. 若收到“新 ACK 但还没越过 recoverq”（部分恢复）：
- 按 `new_data = ackno - _last_acked` 先回收一部分 `_inflate`
- 再 `_inflate += mss()`
- 再次 `retransmit_packet()`（认为可能多包丢）
- `applySwiftLimits()` + `send_packets()`
- 见 `htsim/sim/swift.cpp:220`

3. 若收到“新 ACK 且 ackno >= _recoverq”（恢复结束）：
- `_inflate = 0`
- `_dupacks = 0`
- `_in_fast_recovery = false`
- 可选做一次额外 MD（若 `_can_decrease`）：`_swift_cwnd *= (1 - _max_mdf)`
- 然后 `applySwiftLimits()` + `send_packets()`
- 见 `htsim/sim/swift.cpp:197`

### 4) 和超时（RTO）的交互
- 若 RTO 触发，`doNextEvent()` 会强制退出 FR：
  - `_in_fast_recovery = false`
  - `_recoverq = _highest_sent`
  - `_dupacks = 0`
  - 之后按重传次数做更强降窗（到 `_min_cwnd` 或乘性减）
  - 再 `retransmit_packet()`
- 见 `htsim/sim/swift.cpp:509`

### 5) 对系统其他部分的影响
1. 发送侧节奏：通过 `_inflate` 改变临时可发送窗口（`cwnd + inflate`）
2. 重传行为：FR 和 timeout 都会主动调用 `retransmit_packet()`
3. CC 主环耦合：`adjust_cwnd()` 依旧每 ACK 执行（代码里也提示 “Need to be careful”）
   - `htsim/sim/swift.cpp:88`
4. 状态机与 CC 混合：`handle_ack()` 既包含可靠传输恢复，也会改 cwnd（FR结束时额外 MD）

### 6) 结论（Q8）
- Swift 的 FR 本质是 Reno/NewReno 风格的“3 dupack 快重传 + 恢复窗口”机制。
- 触发条件明确（3 dupack + 恢复门控），退出条件是 ACK 越过 `_recoverq` 或 timeout 打断。
- 它不仅影响重传，还直接影响发送窗口（通过 `_inflate` 与额外 MD），所以与 CC 有实质耦合。

## Q9（已讨论，新增）：Swift 的发包流程是怎样的？`send_packets` / `pacer` / `send_next_packet` 各做什么？

### 1) 先给总图（调用链）
常见入口（都会触发发包尝试）：
1. 连接开始：`startflow()` -> `send_packets()`（`htsim/sim/swift.cpp:717`）
2. ACK 到达：`handle_ack()` 各分支最后 -> `send_packets()`（如 `htsim/sim/swift.cpp:192`）
3. 调度器回调：`send_callback()` -> `send_packets()`（`htsim/sim/swift.cpp:392`）
4. pacing 定时器到点：`SwiftPacer::doNextEvent()` -> `send_next_packet()`（`htsim/sim/swift.cpp:859`）

可把它理解为：
- `send_packets()`：发包主控制器（决定“窗口模式/ pacing 模式/不发”）
- `send_next_packet()`：真正构包并发一个包
- `SwiftPacer`：在“窗口不足 1 包”时按时间驱动反复调用 `send_next_packet()`

### 2) `send_packets()` 全流程（主调度器）
实现位置：
- `htsim/sim/swift.cpp:282`

步骤：
1. 计算可用窗口：`c = _swift_cwnd + _inflate`（`swift.cpp:283`）

2. 建连前分支（`!_established`）  
- 发送 SYN 包：`new_syn_pkt(...)`
- 直接 `p->sendOn()`（`swift.cpp:291`）
- 设置 RTO 计时器
- 返回

3. 小窗口分支（`c < mss()`）  
- 进入 pacing 模式（`swift.cpp:300`）
- 若 pacer 尚未挂起：`_pacer.schedule_send(_pacing_delay)`（`swift.cpp:313`）
- 不直接发数据包，返回

4. 常规窗口分支（`c >= mss()`）  
- 可选 app-limited 限流（`swift.cpp:324`）
- while 循环：只要窗口允许且有数据，反复 `send_next_packet()`（`swift.cpp:335`）
- 如果之前在 pacing 中，先 `cancel()` 切回窗口模式（`swift.cpp:337`）
- 每成功发送包会累计 `sent_count`

### 3) `_pacer.schedule_send()` 在做什么
实现位置：
- `htsim/sim/swift.cpp:829`

它不发包，它只做“定时安排”：
1. 记录 interpacket gap：`_interpacket_delay = delay`
2. 计算下次发送时刻：`_next_send = _last_send + _interpacket_delay`
3. 如果 `_next_send <= now`：立刻执行一次 `doNextEvent()`（即立刻发一包）
4. 否则：把 pacer 事件挂到事件队列（`sourceIsPending`）

所以它的角色是“把发送时机推进到 EventList”，不是发包函数本身。

### 4) `send_next_packet()` 在做什么（这里就是“真正发包”）
实现位置：
- `htsim/sim/swift.cpp:359`

步骤：
1. 先问调度器队列是否允许发送（`queuesize(flow_id) > 2` 则暂不发）：
   - 若不能发：`_deferred_send = true`，返回 `false`（`swift.cpp:361`）

2. 申请下一个 DSN（如果没有更多数据则返回 `false`）：
   - `dsn = _src.get_next_dsn()`（`swift.cpp:371`）

3. 建立重传映射：
   - `_dsn_map[_highest_sent+1] = dsn`（`swift.cpp:377`）

4. 构造数据包：
   - `SwiftPacket::newpkt(..., seq=_highest_sent+1, dsn, size=mss())`（`swift.cpp:379`）

5. 更新发送侧状态：
   - `_highest_sent += mss()`
   - `_packets_sent += mss()`（`swift.cpp:381`）

6. **真正送入网络**：
   - `p->sendOn()`（`swift.cpp:386`）
   - 这是发包动作的关键行

7. 调用 `_pacer.just_sent()` 更新时间基准（`swift.cpp:387`）

### 5) `p->sendOn()` 后还会发生什么（是否继续向下调用）
会。`sendOn()` 不是终点，而是沿 route 推进一跳。

实现：
- `Packet::sendOn()`：`htsim/sim/network.cpp:56`

关键行为：
1. 选取下一跳 `PacketSink`（route 上 `_nexthop` 位置）
2. `_nexthop++`
3. 直接调用 `nextsink->receivePacket(*this)`（`network.cpp:83`）

因此后续会进入队列/pipe/switch/NIC 等对象的 `receivePacket()`，再由它们在后续时刻继续 `sendOn()` 转发。  
也就是说，“真正发包起点在 Swift 的 `p->sendOn()`，网络内部传播由下游组件继续驱动”。

### 6) pacer 如何循环工作
`SwiftPacer::doNextEvent()`（`swift.cpp:859`）：
1. 调 `send_next_packet()` 发一个包
2. 更新 `_last_send`
3. 若当前 `pacing_delay()>0`，再次 `schedule_send(...)`（继续周期发送）
4. 若 `pacing_delay()==0`，退出 pacing 状态（清 `_interpacket_delay/_next_send`）

这就是“定时单包发送 -> 重新定时”的闭环。

### 7) 与调度器背压的配合
- `send_next_packet()` 如果发现调度器队列忙，会返回 false 并设置 `_deferred_send=true`。
- 等调度器可发时会回调 `send_callback()`，再进入 `send_packets()` 重试（`swift.cpp:392`）。

因此发包路径不是“盲发”，而是和 scheduler 有显式 backpressure 协同。

### 8) 结论（Q9）
- `send_packets()` 负责“策略选择与循环控制”；
- `_pacer.schedule_send()` 负责“把下一次发送挂到事件队列”；
- `send_next_packet()` 负责“构包+状态更新+调用 `sendOn()` 真正发包”；
- `sendOn()` 后网络内继续由各 hop 的 `receivePacket()/sendOn()` 链路推进，不在 Swift 里结束。

## Q10（已讨论，新增）：`delay` 是怎么测量的？Dst 何时回 ACK？是每几个包回一次？

### 1) 你的理解是对的：就是 timestamp echo RTT 样本
Swift 的 `delay` 采样流程是：
1. Src 发 data 包时打发送时间戳 `ts`  
  - `p->set_ts(eventlist().now())`：`htsim/sim/swift.cpp:385`
2. Dst（更准确是 `SwiftSubflowSink`）收包后取出该 `ts`  
  - `simtime_picosec ts = p->ts()`：`htsim/sim/swift.cpp:904`
3. Dst 发送 ACK 时把这个 `ts` 回显到 ACK 的 `ts_echo` 字段  
  - `SwiftAck::newpkt(..., ts_echo=ts)`：`htsim/sim/swift.cpp:957`
4. Src 收到 ACK 后读取 `ts_echo`，计算  
  - `delay = now - ts_echo`：`htsim/sim/swift.cpp:459`

所以这就是“Src 发包时刻 -> ACK 返回 Src 时刻”的 RTT 样本（不是 one-way delay）。

### 2) Dst 侧 ACK 触发条件是什么
在当前 `swift.cpp` 实现里，`SwiftSubflowSink::receivePacket` 的最后 **无条件调用** `send_ack(ts)`：
- `htsim/sim/swift.cpp:950`

也就是说：
- 只要收到一个 data 包（不管顺序/乱序/重传），都会发一个 ACK。
- 没有 delayed-ACK 计数器，也没有“每 N 包回一次”的逻辑。

### 3) ACK 中 ACKNO 的语义
- ACK 带的是 `_cumulative_ack`（子流累计确认点），不是“仅确认刚收这个包”：
  - `htsim/sim/swift.cpp:957`
- 即使 ack 是每包都发，它依然是累计 ACK 语义。

### 4) ACK 是否会因为 out-of-order 改变回包频率
- 不会改变“是否发 ACK”这个频率：仍是每收一包就回 ACK。
- 变化的是 ACK 里的 `ackno` 数值推进方式：
  - in-order 时推进快；
  - 有洞时 `ackno` 可能停住，但 ACK 仍持续发送（导致 dupack）。

对应逻辑：
- `SwiftSubflowSink::receivePacket` 对 `_cumulative_ack/_received` 的维护：
  - `htsim/sim/swift.cpp:911` 起

### 5) 这个 delay 样本有个细节
注释明确说：
- “echoed TS is always from the packet we just received”
- `htsim/sim/swift.cpp:948`

含义：
- ACK 的 `ackno` 可能是累计确认点；
- 但 `ts_echo` 始终来自“刚收到的那一包”，不是累计点对应包的 ts。

这会让“ACK语义”和“delay样本对应哪一包”在乱序/重传场景下不完全对齐。

### 6) 结论（Q10）
- `delay` 测量机制就是 timestamp echo RTT：`delay = now - ts_echo`。
- Dst 当前实现是 **每收到一个 data 包就发一个 ACK**（无 delayed ACK / 无 N 包一 ACK）。
- ACK 是累计确认语义，但 delay 样本绑定“刚收到的包”的 ts，这一点在复杂场景下要特别注意。
