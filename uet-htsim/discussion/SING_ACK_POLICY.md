# SING ACK Policy 接口说明（含 RepsAckAgg 示例）

> 面向当前代码：`htsim/sim/sing_sink.h/.cpp`（`processData` 单入口 + 策略化 ACK 反馈）

## 1. 为什么引入 ACK Policy

`SingSink::processData` 的大部分阶段（收包校验、新包统计、ACK 发送收尾）在不同模式下是共享的，变化点主要集中在“ACK 何时发、ACK 带什么反馈”。  
因此现在用 `AckPolicy` 把模式差异收敛到同一组接口，`processData` 保持固定骨架。

---

## 2. 接口位置与运行时绑定

### 2.1 接口定义

- 文件：`htsim/sim/sing_sink.h`
- 抽象类：`SingSink::AckPolicy`
- 当前实现：
  - `SingSink::LegacyAckPolicy`
  - `SingSink::RepsAckAggPolicy`

接口方法：

1. `onDuplicate(const SingDataPacket&, int sf_id, bool ecn) -> SingAck*`
2. `onNewPacket(const SingDataPacket&, seq_t dsn, bool ecn, bool& force_ack)`
3. `ackSendCondition(bool ecn, bool threshold_reached, bool force_ack) -> bool`
4. `buildAck(const SingDataPacket&, seq_t dsn, int sf_id, bool ecn) -> SingAck*`
5. `beforeSendAck(SingAck*)`

### 2.2 运行时选择策略

- 构造阶段创建两个策略对象并绑定默认模式：
  - `SingSink::SingSink(...)`
  - `setAckFeedbackMode(_ack_feedback_mode)`
- 切换函数：
  - `SingSink::setAckFeedbackMode(AckFeedbackMode mode)`
  - 根据 `LEGACY / REPS_ACKAGG` 选择 `_ack_policy`。

---

## 3. `processData` 与策略的调用关系

当前 `processData` 是固定 6 阶段：

1. 校验与基础统计（ECN stats、debug log）
2. duplicate 快路径：`onDuplicate` -> `beforeSendAck` -> send -> reset
3. 新包统计（`accountNewDataPacket`）
4. 状态更新：`onNewPacket`
5. ACK 判定与构建：`ackSendCondition` + `buildAck` + `beforeSendAck`
6. 发 ACK 与计数器 reset

这保证“流程骨架稳定、差异点集中在策略类”。

---

## 4. RepsAckAgg 作为策略示例

`reps_ackagg` 模式的核心是：ACK 可携带聚合反馈，且 ECN 不再立即触发 ACK。

### 4.1 Sink 侧差异（`RepsAckAggPolicy`）

1. `onNewPacket(...)`
   - 调用 `maybeMarkForceAckByFlags(..., false)`：关闭 `path_sel_end` 立即 ACK。
   - 调用 `updateOooState(...)` 维护 OOO/ack_request 状态。
   - 调用 `noteAggFeedbackFromNewPacket(...)` 聚合 EV：
     - 非 ECN：追加到 `_pending_non_ecn_evs_since_last_ack`
     - ECN：仅置 sticky ECN（记录首个 ECN EV）

2. `ackSendCondition(...)`
   - `return threshold_reached || force_ack;`
   - 即：`ecn` 本身不再立即触发 ACK。

3. `beforeSendAck(...)`
   - 调用 `attachAggFeedbackToAckAndFlush(...)`：
     - 将聚合窗口写入 ACK 元数据
     - flush pending 状态

4. duplicate 路径
   - `onDuplicate(...)` 仍立即回 ACK
   - 若 duplicate 包带 ECN，会保持 sticky ECN 状态，避免反馈丢失

### 4.2 ACK 元数据字段（仿真侧）

在 `SingAck` 中新增：

- `agg_non_ecn_evs`
- `agg_ecn_ev_valid`
- `agg_ecn_ev`

注意：这些字段是 simulation metadata，不计入 `ACKSIZE`，`newpkt()` 会清空避免 packet DB 复用污染。

### 4.3 Src 侧配合

`SingSrc::processAckPathFeedback(...)` 中：

- 若 `_reps_ackagg_mode`：
  - ECN：`processEv(PATH_ECN)`（优先用 `agg_ecn_ev`，无效时回退 `pkt.ev()`）
  - 遍历 `agg_non_ecn_evs`，逐项 `processEv(PATH_GOOD)`
  - 直接 `return`，跳过 legacy 单 ACK 单 EV 路径，避免双计数

---

## 5. 模式启用路径（main_sing）

1. CLI：`-load_balancing_algo reps_ackagg`
2. fail-fast 约束：
   - `pathwise_subflows == 1`
   - `path_selection_granularity == MTU`
3. 连接创建时同时设置：
   - `uec_src->setRepsAckAggMode(true)`
   - `uec_snk->setAckFeedbackMode(SingSink::AckFeedbackMode::REPS_ACKAGG)`

---

## 6. 新增 ACK Policy 的建议步骤

1. 在 `SingSink::AckFeedbackMode` 增加新枚举。
2. 在 `sing_sink.h` 增加新策略类声明（继承 `AckPolicy`）。
3. 在 `sing_sink.cpp` 实现 5 个接口方法。
4. 构造函数中创建策略对象；在 `setAckFeedbackMode` 里挂接 `_ack_policy`。
5. 保持 `processData` 骨架不变，只通过策略方法注入差异。
6. 若需要源端配合，在 `SingSrc::processAckPathFeedback` 增加对应分支。

这样可以保证后续新增 policy 时改动面可控，不会再次把 `processData` 膨胀回巨型分支函数。

