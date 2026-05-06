# Phase C Swift 接入步骤记录

## Step 1: 在 `BaseCC` 框架中新增 `Swift` 类
- 文件：
  - `htsim/sim/sing_cc.h`
  - `htsim/sim/sing_cc.cpp`
- 结果：
  - 新增 `Swift::Config` 与 `Swift` 实现类
  - 首版仅保留 delay-based 控制律，不包含 fast recovery 状态机
  - `target_delay` 采用 `base_rtt + fs_delay(cwnd)`（方案 B）
  - `fs_delay` 公式与 `swift.cpp` 原始实现保持一致（`1/sqrt(cwnd/pkt_size)` 形态 + clamp）

## Step 2: 接入 `SingSrc` 的 CC 选择链路
- 文件：
  - `htsim/sim/sing.h`
  - `htsim/sim/sing.cpp`
  - `htsim/sim/datacenter/main_sing.cpp`
- 结果：
  - `Sender_CC` 新增 `SWIFT`
  - `-sender_cc_algo` 新增 `swift`
  - `initCcForFlow` 新增 `SWIFT` 分支并实例化 `Swift`
  - 每连接 `base_rtt` 注入 Swift 作为 `target_delay` 基线项

## Step 3: 最小回归加入 Swift case
- 文件：
  - `htsim/sim/datacenter/lb_explore/cc_regression_4cases.json`
  - `htsim/sim/datacenter/lb_explore/AAAreadme.txt`
- 结果：
  - 在现有回归集合中新增 `swift` case（保持原配置体系不变）
  - readme 更新为 5 个 baseline case

## Step 4: 构建与回归验证
- 状态：已完成
- 结果：
  1. 编译通过：`htsim_sing`
  2. 回归通过：`python3 exp_runner.py -c lb_explore/cc_regression_4cases.json`
  3. 输出中已包含 `swift` case：
     - `htsim/sim/datacenter/lb_results/cc_regression_4cases/cc_regression_summary.md`
     - `htsim/sim/datacenter/lb_results/cc_regression_4cases/cc_regression_summary.csv`

## Step 5: Swift 方法调用与字段使用状态（review 对照）

### 5.1 方法调用时机（当前代码）
1. `Swift::onAck(delay, acked_bytes, ecn, in_flight)`
- 调用点：`SingSrc::processAck()` -> `_cc->onAck(...)`
- 代码：`htsim/sim/sing.cpp:1202`
- 调用条件：`_sender_based_cc == true` 且 `_cc` 已实例化（包括 Swift）
- 当前行为：
  - RTT 平滑更新（`updateRtt`）
  - 计算 `target_delay = base_target_delay + fs_delay(cwnd)`
  - `raw_rtt < target_delay` -> AI；否则（满足门控）MD
  - `setCwndBounds()`

2. `Swift::onNack(nacked_bytes, last_hop, in_flight)`
- 调用点：`SingSrc::processNack()` -> `_cc->onNack(...)`
- 代码：`htsim/sim/sing.cpp:1527`
- 调用条件：收到 NACK 且 sender-based CC 开启
- 当前行为：
  - 无 FR 状态机
  - 只做一次可门控 MD（每 RTT 最多一次）

3. `Swift::onTimeout(in_flight)`
- 调用点：`SingSrc::rtxTimerExpired()` -> `_cc->onTimeout(...)`
- 代码：`htsim/sim/sing.cpp:2152`
- 调用条件：发送端 RTO 超时路径触发
- 当前行为：
  - `_retransmit_cnt++`
  - 达阈值 -> `cwnd=min_cwnd`
  - 未达阈值 -> 尝试一次 MD
  - `setCwndBounds()`

### 5.2 字段使用状态（当前 Swift 实现）
#### A. 当前“活跃使用”字段
- `_cwnd`：窗口输出核心状态（调度读取）
- `_base_rtt`：构造 `raw_rtt` 与目标时延基线
- `_rtt`：用于“每 RTT 最多一次减窗”门控
- `_last_decrease`：减窗门控时间戳
- `_retransmit_cnt`：timeout 回退阈值计数
- `_fs_range/_fs_alpha/_fs_beta`：`fs_delay(cwnd)` 计算参数
- `_cfg.*`：运行参数源（`min/max/init/pkt_size/ai/beta/max_mdf/rtx_reset_threshold/fs_*`）

#### B. 当前“更新但未被外部消费”（预留/诊断）字段
- `_rto`
- `_min_rto`
- `_mdev`

说明：
- 这些字段在 `updateRtt()` 中会被更新，保留了 Swift 原始 RTT/RTO 估计骨架；
- 但当前 Phase C 中，Sing 的超时调度仍由 `SingSrc` 负责，`Swift::_rto` 不直接驱动定时器。
