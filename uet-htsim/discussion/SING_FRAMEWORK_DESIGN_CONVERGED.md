# Sing Framework 设计收敛稿（精简版）

> 2026-03 文件拆分说明：原 `sing.cpp/sing.h` 已拆分为 `sing_src.cpp|sing_sink.cpp|sing_nic.cpp` 与对应头文件，本文中的旧行号引用已改为稳定的函数名+文件名口径。

**版本**: v0.2  
**日期**: 2026-02-17  
**状态**: Draft（以当前代码为准）

快速入口：
- [discussion/SING_QUICKSTART_GUIDE.md](discussion/SING_QUICKSTART_GUIDE.md)（面向 coding agent / user 的上手文档，优先看这里建立 As-Is 认知）

---

## 1. 文档目标

本稿用于收敛讨论，明确三件事：

1. 当前代码里已经实现了什么（事实清单）
2. 哪些能力尚未实现但计划实现（缺口清单）
3. 近期实施方向和顺序（路线清单）

说明：
- 本文不替代原设计文档 `discussion/SING_FRAMEWORK_DESIGN.md`
- 原文档保留，本文只做“现状对齐 + 下一步执行”

---

## 2. 已确认决策（Decision Log）

1. CC 路线：**B**  
先做 CC 抽象（`BaseCC` 插件化）再推进 Subflow/Swift/PLB。

2. Sleek 定位：**A**  
Sleek 视为**已实现能力**（实验/可选特性），不再作为远期空白能力管理。

3. PFC 路线：**复用现有实现**  
优先复用现有 `ETH_PAUSE` / `LosslessQueue` / 交换机 pause 处理链路做增强，不新开并行协议栈。

4. 文档策略：**先出精简版收敛稿**  
保留原文档不删，新增本文档承接当前迭代。

5. 实施顺序调整：**先 Swift，后 Subflow**  
在现有单连接/单发送上下文下先接入 `Swift` 到 `BaseCC`，稳定后再推进 subflow + path-wise。

---

## 3. 当前实现（As-Is，基于代码）

## 3.1 传输与可靠性
- 已有：RTO 重传、NACK 触发重传、SACK 位图 ACK、Trim 包处理
- 相关代码：
  - `htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
  - `htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
  - `htsim/sim/singpacket.h`

## 3.2 多路径（Native LB）
- 已有：`SingMultipath` 及多种策略（Oblivious / Bitmap / REPS / Mixed）
- 相关代码：
  - `htsim/sim/sing_mp.h`
  - `htsim/sim/sing_mp.cpp`
  - `htsim/sim/datacenter/main_sing.cpp`

## 3.3 拥塞控制（当前形态）
- 已完成 `BaseCC` 插件式抽象（`NSCC/DCTCP/CONSTANT`）
- `SingSrc` 不再维护 `_cwnd`，调度统一从 CC 读取 window/rate
- 相关代码：
  - `htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
  - `htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
  - `htsim/sim/sing_cc.h`
  - `htsim/sim/sing_cc.cpp`
  - `htsim/sim/datacenter/main_sing.cpp`

## 3.4 Sleek（已实现能力）
- 已有开关与主逻辑（`-sleek`、`runSleek()`、ACK 携带 OOO 计数）
- 相关代码：
  - `htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
  - `htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
  - `htsim/sim/singpacket.h`
  - `htsim/sim/datacenter/main_sing.cpp`

## 3.5 PFC 相关基础（可复用）
- 已有：`ETH_PAUSE` 包类型、`LosslessQueue`、交换机层 pause 处理入口
- 相关代码：
  - `htsim/sim/eth_pause_packet.h`
  - `htsim/sim/queue_lossless.h`
  - `htsim/sim/queue_lossless.cpp`
  - `htsim/sim/datacenter/fat_tree_switch.cpp`

---

## 4. 缺口清单（To-Be，尚未实现）

1. **Swift CC（在现有框架内先接入）**
- 作为 `BaseCC` 子类接入，不要求先有 subflow
- 第一阶段只做连接级 sender-cc（复用当前 Sing 可靠传输和调度链路）

2. **Path-wise CC LB / Subflow**
- 引入 subflow 实体与固定路径绑定
- 明确 per-subflow CC 上下文及与可靠传输关系

3. **PLB**
- 基于路径状态与时延反馈触发迁移，依赖 subflow 结构

4. **PFC 增强**
- 在现有 pause 机制上增强可配置性、统计和验证，而非平行重建

5. **Sleek 与 NSCC 目标时延耦合清理（TODO）**
- 当前 `runSleek()` 仍依赖 `SingSrc::_target_Qdelay`（该值由 NSCC 初始化阶段回填）。
- 需要后续梳理并解耦：明确 Sleek 应依赖 NSCC 内部接口还是独立参数，避免隐式跨模块依赖。

---

## 5. 近期路线（执行顺序）

## Phase A：文档与口径统一（当前阶段）
- 目标：消除“文档说法”和“代码事实”不一致
- 输出：
  - 本收敛稿（当前文件）
  - 后续补充一份参数/开关对照表（代码事实版）

## Phase B：CC 抽象重构（已完成）
- 状态：已落地（`BaseCC` + `NSCC/DCTCP/CONSTANT`）
- 验证：已有 `cc_regression_4cases` 可用于快速回归

## Phase C：Swift 接入（当前下一步）
- 目标：在现有 sender-cc 框架中新增 `Swift`（先不引入 subflow）
- 任务：
  - 明确 `Swift` 参数集及默认值（`base_delay/hdiv/ai/beta/max_mdf/min_cwnd/max_cwnd/rtx_reset_threshold`）
  - 在 `sing_cc.*` 增加 `Swift` 实现类，统一走 `BaseCC` 接口
  - 在 `main_sing.cpp` 扩展 `-sender_cc_algo swift`
  - 扩展回归 case（在现有 4-case 基线基础上增加 swift case）

### Phase C 约束与决策（讨论收敛）
1. 仅接入 delay-based 控制律，**不接入 Fast Recovery 机制**  
- 不迁移 `_inflate/_dupacks/_recoverq/_in_fast_recovery` 等状态机逻辑。
- 维持 Sing 现有可靠传输/重传路径不变。

2. RTT 采样复用 Sing 现有方式  
- Swift-CC 的 `delay` 输入不依赖逐包 ACK。
- 直接复用当前 Sing/NSCC 已有 ACK 时延测量口径（sender 收 ACK 时基于发送时间记录计算样本）。

3. `target_delay` 的基线项改为“连接 RTT 基线”，不强依赖 hop-count 项  
- Swift 原实现中 `base_delay + hop_delay`，在 Sing 中改为以 `src-dst` 连接基线 RTT（或其固定比例）作为目标基线。
- 现阶段不引入独立 per-hop 参数；后续若 needed 再补可选项。

4. CC 参数输入机制从 CLI 逐步升级为配置文件  
- 短期（Phase C 首版）：保留 CLI 最小开关（只选算法），参数用内置默认值。
- 中期：新增 `--cc_profile <json>`（或等价）输入，按算法读取参数块。
- 目标：避免 `main_sing.cpp` 持续膨胀为“所有 CC 参数全集”。

### Phase C 实施顺序（代码层）
1. 在 `sing_cc.*` 新增 `Swift`（仅 delay-based AIMD 主环，含 `target_delay` 计算与窗口边界）。
2. 在 `SingSrc::initCcForFlow` 增加 `swift` 分支并注入每连接基线 RTT。
3. 在 `main_sing.cpp` 开放 `-sender_cc_algo swift`。
4. 增加最小回归 case：`swift` + 现有 4-case 并列，先验证“可跑通+无异常”。
5. 第二步再加参数文件解析，不阻塞首版落地。

## Phase D：Subflow + Path-wise
- 目标：引入 subflow 结构并支持 path-wise 调度
- 前置：Phase C 完成并稳定

## Phase E：PLB
- 目标：基于路径状态与时延反馈触发迁移
- 前置：Phase D 完成

## Phase F：PFC 增强（复用现有）
- 目标：在现有 `ETH_PAUSE/LosslessQueue` 机制上补齐参数、监控、验证

---

## 6. 边界与不做项（当前版本）

- 不在本轮收敛稿中重写原设计全文
- 不在本轮直接承诺每个 Phase 的代码行数估计
- 不在本轮引入新协议类型替代现有 `ETH_PAUSE`

---

## 7. 下一步建议（讨论后可立即执行）

1. 固化 Swift 接入基线：确认采用 `htsim/sim/swift.cpp` 的算法口径，不采用 `swift_new.*`
2. 定义 `Swift` 的 `BaseCC` 映射（ACK/NACK/Timeout 输入与状态输出）
3. 给 `cc_regression_4cases` 增加 `swift` case，作为 Phase C 验收入口
