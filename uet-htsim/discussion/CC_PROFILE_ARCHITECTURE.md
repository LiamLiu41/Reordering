# CC Profile 架构设计

> 2026-03 文件拆分说明：原 `sing.cpp/sing.h` 已拆分为 `sing_src.cpp|sing_sink.cpp|sing_nic.cpp` 与对应头文件，本文中的旧行号引用已改为稳定的函数名+文件名口径。

状态：In Progress（R1 已落地；flow 级 CC/Profile 覆盖已落地；R2/R3 仍有扩展项）

---

## 0. 文档维护规则

- 文档主结构固定为：`需求定义`、`方案设计`、`实现`。
- 在“开始实现前”，`实现`章节保持为空（或仅写“未开始”）。
- 每条需求必须标注状态：`Done / TODO / Pending`。
- 需求描述必须包含：`场景`、`目标`、`验收条件`。
- 方案描述必须包含：
  - `设计动机`（为什么需要这个类/函数）
  - `字段/参数语义`（每个成员变量是什么意思）
  - `调用路径`（准备从哪里调用，到哪里生效）
- 每次需求或方案发生变化，先改文档，再改代码。

---

## 1. 需求定义

### 1.1 Feature 起点（cc_profile 到底是什么）
`cc_profile` 是一个“CC 参数配置文件”能力：
- 用文件集中描述各类 CC（nscc/swift/dctcp/constant）的参数模板；
- 允许同一种 CC 提供多套参数组；
- 在建 flow 时，结合运行时变量（如 RTT/BDP）得到该 flow 的最终参数；
- 目标是替代“参数分散在 main 和代码常量里”的方式。

### 1.2 需求清单

#### R0 保留现有入口与默认行为（状态：Done）
场景：现有实验命令通过 `-sender_cc_algo` 运行，不能被破坏。
目标：即使没接入 profile 文件，也能保持当前行为。
验收条件：原命令行脚本不改即可跑通。

#### R1 统一 CC 初始化路径（状态：Done）
场景：当前既有全局初始化，又有每 flow 初始化，责任边界不清晰。
目标：每 flow 的最终 CC 参数落地统一到一个入口。
验收条件：所有 sender-based CC 最终实例化都走同一条 flow 级路径。

#### R2 一次仿真支持多组 CC 配置（状态：In Progress）
场景：同一次仿真中，不同 flow 可能选不同 CC，或同 CC 不同参数组。
目标：profile 文件支持“多 CC + 同 CC 多参数组”。
验收条件：能表达如 `swift_default` 与 `swift_conservative` 并存；并可在 `.cm` 对单 flow 显式指定 `cc_algo/cc_profile`。

#### R3 运行时变量参与参数求值（状态：TODO）
场景：某些参数需要在建 flow 时计算，例如 `max_cwnd = k * bdp_bytes`。
目标：定义统一 runtime 变量集合，并可被 profile 参数引用。
验收条件：参数求值能读取 flow 级 RTT/BDP/MTU/MSS/速率。

#### R4 参数互依赖表达能力（状态：Pending）
场景：参数之间存在依赖链，例如 `B=A*3`, `C=A*B`。
目标：支持参数间依赖表达与求值。
验收条件：可检测循环依赖并报错。

#### R5 统一 CC 事件输入（状态：Pending）
场景：不同 CC 的 `onAck/onNack/onTimeout` 输入形态不统一。
目标：统一事件输入模型，降低后续新增 CC 成本。
验收条件：事件对象可覆盖 ACK/NACK/TIMEOUT 所需字段。

#### R6 复杂绑定规则层（状态：Pending）
场景：未来可能按 flow 类型/标签自动选择 profile。
目标：提供 `binding` 规则层（如 `default`、`by_flow_type`）。
验收条件：规则层可与“flow 显式指定”共存并定义优先级。

---

## 2. 方案设计

### 2.1 对应 R1：初始化分层与职责边界

#### 2.1.1 设计动机
当前初始化逻辑中，全局量和 flow 量混在一起，导致“哪个参数何时定值”不透明。需要分层：
- 全局层：只管网络共享默认量；
- flow 层：只管某条 flow 的 runtime 上下文；
- 实例化层：只管把参数落地为具体 CC 实例。

#### 2.1.1A Flow 层与实例化层的边界（明确版）
- Flow 层（`FlowBasicParams` 构造阶段）只做“事实收集与统一打包”：
  - 读取这条 flow 已知的运行时事实（`peer_rtt`、发送 NIC 速率、MTU/MSS、全局 fallback）。
  - 产物是一个“输入上下文对象”，不包含任何算法决策。
  - 不做：不选 CC、不算最终参数、不创建对象。
- 实例化层（`initCcForFlow` 落地阶段）只做“算法决策与对象创建”：
  - 读取 Flow 层上下文 + profile 参数。
  - 完成参数求值（常量/表达式）并填充 `Nscc::Config/Swift::Config/...`。
  - 创建并挂载具体 CC 实例。
- 一句话：
  - Flow 层回答“已知输入是什么”；
  - 实例化层回答“基于输入创建哪个 CC、参数是多少”。

#### 2.1.2 设计对象 A：`GlobalNetworkParams`（计划新增类）
动机：把所有网络级共享默认量集中存储，避免散落静态变量。

字段语义（首版建议）：
- `network_rtt_ps`：网络级参考 RTT。
- `network_bdp_bytes`：网络级参考 BDP。
- `network_linkspeed_bps`：网络级参考速率（全局 fallback）。
- `trimming_enabled`：网络是否启用 trimming（供 CC 在类内决定默认阈值策略）。
- `default_mtu_bytes`：全局默认 MTU。
- `default_mss_bytes`：全局默认 MSS。

`network_linkspeed_bps` 细化说明：
- 源头：
  - 来自程序启动阶段的全局链路速率配置（当前主路径是 `main_sing.cpp` 里的 `linkspeed`）。
  - 由 `initCcGlobalDefaults(...)` 写入一次。
- 用途：
  - 作为“网络级默认速率”参与全局参考量计算（例如 `network_bdp_bytes`）。
  - 在某些 flow 缺少明确 NIC 速率信息时做 fallback（理论兜底）。
- 不用于：
  - 不直接代表某条具体 flow 的发送速率；具体 flow 仍以 `nic_linkspeed_bps` 为准。

调用路径（计划）：
- 写入：程序启动后，由全局初始化函数写入一次。
- 读取：建 flow 时，用于构造 flow runtime 上下文的 fallback 字段。

#### 2.1.3 设计对象 B：`FlowBasicParams`（计划新增类）
动机：把“每 flow 的运行时输入变量”打包成统一上下文，供参数求值和实例化使用。

字段语义（首版建议）：
- `peer_rtt_ps`：该 flow 的 pair RTT（flow 级优先）。
- `bdp_bytes`：该 flow 的 BDP（flow 级优先）。
- `nic_linkspeed_bps`：该 flow 对应发送 NIC 的实际速率（flow 级优先）。
- `mtu_bytes`：该 flow 使用的 MTU。
- `mss_bytes`：该 flow 使用的 MSS。

`nic_linkspeed_bps` 细化说明：
- 源头：
  - 来自该 flow 绑定的发送端 NIC 对象（例如 `src->_nic.linkspeed()`）。
  - 是“这条 flow 在当前发送端口上的实际速率”。
- 用途：
  - 用于计算 flow 级 `bdp_bytes`（典型：`bdp_bytes = peer_rtt_ps * nic_linkspeed_bps / 8`）。
  - 用于 window/rate 互转时的 flow 级换算基准。
- 优先级：
  - 参数求值时，flow 级速率优先使用 `nic_linkspeed_bps`；
  - 只有缺失 flow 级速率时才退化为 `network_linkspeed_bps`（来自 `GlobalNetworkParams`）。

两者关系（`nic_linkspeed_bps` vs `network_linkspeed_bps`）：
- 在当前同构拓扑下，两者通常数值相同，但语义不同。
- `nic_linkspeed_bps` 是“具体 flow 的真实输入”，`network_linkspeed_bps` 是“网络级参考/兜底输入”。

调用路径（计划）：
- 构造：`main_sing.cpp` 基于 flow RTT/flow BDP 调用 `buildFlowBasicParams(...)`。
- 使用：CC 参数求值阶段读取该结构。

#### 2.1.4 设计对象 C：`initCcGlobalDefaults(...)`（计划新增函数）
动机：把“全局默认量初始化”从 CC 实例化逻辑里拆开。

输入：
- `network_rtt`、`network_bdp`、`linkspeed`、`trimming_enabled`。

输出：
- 更新 `GlobalNetworkParams`。

调用路径（计划）：
- 在程序启动后、建 flow 前调用一次（由 `main_sing.cpp` 直接调用）。

#### 2.1.5 设计对象 D：`initCcForFlow(...)`（改造现有函数）
动机：成为“每 flow 最终落地”的唯一入口。

职责：
- 接收 `cwnd` 与 `FlowBasicParams`；
- 选择 CC 算法；
- 基于 runtime + profile 参数，生成最终配置并实例化。

调用路径（计划）：
- 由 `main_sing.cpp` 在每条 flow 创建时调用。

#### 2.1.6 CC 类内参数初始化（关键约束）
动机：CC 私有参数应由对应 CC 类维护和计算，避免 `SingSrc` 维护一份重复参数。

规则：
- `SingSrc` 只负责传递三类输入：`GlobalNetworkParams`、`FlowBasicParams`、`CC profile`（后续接入）。
- 所有 CC 统一实现 `BaseCC::initCcParams(GlobalNetworkParams, FlowBasicParams, CcProfile, init_cwnd)`。
- `CcProfile` 是通用字典结构（`params[key]`），而不是每个 CC 各自一套 profile struct。
- `SingSrc` 在 `switch(_sender_cc_algo)` 里只做“调用对应类的 `initCcParams` + 构造对象”，不再手算 NSCC 私有派生参数。

#### 2.1.7 Profile 与 Config 的职责边界（避免混淆）
动机：同一算法里，外部输入模板和最终运行参数不是一个层级，必须分层。

规则：
- `Profile`：用户输入侧模板（通用字典 + 表达式），用于表达“想要什么”。
- `Config`：某个具体 CC 的运行时内部参数对象（仅在 CC 类内部维护），用于表达“最终怎么跑”。
- `initCcParams(...)`：唯一转换点，负责 `CcProfile + GlobalNetworkParams + FlowBasicParams -> CC内部Config`。

调用路径：
- `main_sing.cpp` 先确定算法。
- `SingSrc::initCcForFlow(...)` 组装该算法的 `Profile`（现阶段仍以代码默认值为主）。
- 调用算法类 `initCcParams(...)` 产出 `Config`，再构造 CC 实例。

---

### 2.2 对应 R2：cc_profile 文件模型

#### 2.2.1 设计动机
把“同类 CC 多参数组”和“默认参数映射”从代码硬编码中抽离到文件。

#### 2.2.2 结构建议
- `cc.defaults`：`algo -> default_profile_id`
- `cc.profiles`：`profile_id -> {algo, params}`

示例（简化）：
```json
{
  "cc": {
    "defaults": {
      "nscc": "nscc_default",
      "swift": "swift_default"
    },
    "profiles": {
      "swift_default": {"algo": "swift", "params": {"beta": 0.8}},
      "swift_conservative": {"algo": "swift", "params": {"beta": 0.9}}
    }
  }
}
```

#### 2.2.3 调用路径（计划）
- 程序启动时读取 profile 文件。
- flow 未显式指定 profile 时：`main.sender_cc_algo -> cc.defaults[algo]`。
- flow 显式指定 profile（后续阶段）时优先使用显式项。

#### 2.2.4 `.cm` flow 级覆盖（已实现）

功能点：
- `.cm` 的 flow 行支持两个新 token：
  - `cc_algo <nscc|dctcp|constant|swift>`
  - `cc_profile <profile_id>`
- 覆盖优先级：
  1. flow 指定 `cc_profile`：优先使用该 profile；若同时写 `cc_algo`，必须与 profile 的 `algo` 一致，否则启动即报错。
  2. flow 仅指定 `cc_algo`：算法使用 flow 指定值；参数 profile 使用该算法在 `cc.defaults[algo]` 的默认 profile（若存在），否则回退算法内置默认。
  3. flow 未指定：沿用全局 CLI（`-sender_cc_algo` + `-cc_profile`）。
- `conn_reuse` 约束：
  - 同一 `flowid` 的首条连接定义可写 `cc_algo/cc_profile`；
  - 后续消息行若再写 cc 字段，直接报错。

最小 `.cm` 示例：

```text
Nodes 2
Connections 3
0->1 start 0 size 10000 id 101 cc_algo dctcp
0->1 start 0 size 10000 id 102 cc_profile swift_test
0->1 start 0 size 10000 id 103 cc_algo swift cc_profile swift_test
```

对应 CLI 示例：

```bash
./htsim_sing \
  -tm /tmp/flow_cc_demo.cm \
  -sender_cc_algo nscc \
  -cc_profile htsim/sim/datacenter/cc_profiles/test_overrides.json \
  -end 10
```

说明：
- 上面 3 条 flow 会分别使用：`dctcp/default(dctcp)`、`swift_test`、`swift_test`。
- 未写 cc token 的 flow 会继续使用全局 `nscc`（及其默认 profile）。

---

### 2.3 对应 R3：参数求值与 runtime 变量使用

#### 2.3.1 设计动机
让 profile 可以表达“固定常量 + runtime 计算量”混合参数。

#### 2.3.2 规则建议（首版）
- 常量参数：直接使用。
- 表达式参数：使用 `expr`，变量从 `FlowBasicParams` 读取。

示例：
```json
{
  "max_cwnd": {"expr": "1.5 * bdp_bytes"},
  "base_target_delay": {"expr": "peer_rtt_ps"}
}
```

#### 2.3.3 调用路径（计划）
- 在 `initCcForFlow` 内，profile 选定后执行参数求值。
- 求值结果写入 `Nscc::Config/Swift::Config/...` 后再实例化。

---

### 2.4 对应 R4（Pending）：参数互依赖

#### 2.4.1 设计动机
支持更复杂参数关系，减少重复配置。

#### 2.4.2 规划
- 支持参数引用参数。
- 引入依赖图排序与循环检测。
- 此项 Pending，不进入首版实现。

---

### 2.5 对应 R5（Pending）：统一事件输入

#### 2.5.1 设计动机
统一 `onAck/onNack/onTimeout` 输入语义，减少 CC 接入成本。

#### 2.5.2 规划
- 设计 `FlowRuntimeEvent`。
- 覆盖 ACK/NACK/TIMEOUT 必要字段。
- 此项 Pending，不进入首版实现。

---

### 2.6 对应 R6（Pending）：绑定规则层

#### 2.6.1 设计动机
未来支持按 flow 类型/标签做 profile 路由。

#### 2.6.2 规划
- 增加 `binding` 配置层（如 `default`, `by_flow_type`）。
- 定义优先级：`flow显式 > binding规则 > defaults映射`。
- 此项 Pending，不进入首版实现。

---

## 3. 实现

### 3.1 当前实现状态
- 已开始实现。
- 当前已完成：R1（初始化分层与统一落地点）。
- 当前已完成：R2 基础能力（`cc_profile` 文件加载 + `.cm` flow 级 `cc_algo/cc_profile` 覆盖）。
- 当前已完成：R3 基础能力（profile 表达式求值，支持 runtime 变量）。
- 当前待扩展：R2/R3 的规则层与更复杂表达能力（见上文 Pending 项）。

### 3.2 R1 已落地内容

实现项 A：新增全局默认结构 `GlobalNetworkParams`
- 位置：`htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
- 字段：`network_rtt_ps`、`network_bdp_bytes`、`network_linkspeed_bps`、`trimming_enabled`、`default_mtu_bytes`、`default_mss_bytes`
- 使用：由 `SingSrc::_global_network_params` 持有，供 flow 参数构造与 CC 配置计算使用。

实现项 B：新增 flow 上下文结构 `FlowBasicParams`
- 位置：`htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
- 字段：`peer_rtt_ps`、`bdp_bytes`、`nic_linkspeed_bps`、`mtu_bytes`、`mss_bytes`
- 使用：作为 `initCcForFlow` 新落地点的统一输入上下文。

实现项 C：新增全局初始化函数 `initCcGlobalDefaults(...)`
- 声明位置：`htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
- 定义位置：`htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
- 调用路径：`main_sing.cpp -> initCcGlobalDefaults(...)`
- 作用：统一写入网络全局事实，不再写入 NSCC 私有派生参数。

实现项 D：新增 flow 参数构造函数 `buildFlowBasicParams(...)`
- 声明位置：`htsim/sim/sing_src.h/sing_sink.h/sing_nic.h`
- 定义位置：`htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
- 调用路径：`main_sing.cpp` 在建流时直接调用，再传入 `initCcForFlow(...)`。
- 作用：把 flow 级 RTT/BDP 与 NIC/MTU/MSS 打包成统一上下文。

实现项 E：`initCcForFlow` 统一落地点
- 新函数：`initCcForFlow(mem_b cwnd, const FlowBasicParams& params)`
- 位置：`htsim/sim/sing_src.cpp/sing_sink.cpp/sing_nic.cpp`
- 调用关系：由 `main_sing.cpp` 直接传入 `FlowBasicParams` 调用。
- 结果：NSCC/DCTCP/CONSTANT/SWIFT 的每 flow 实例化统一到一个函数体内。
- 约束：`SingSrc` 仅分发，不在分支里手算 NSCC 私有派生参数。

实现项 F：每个 CC 类新增 `initCcParams(...)`
- 位置：
  - `htsim/sim/sing_cc.h`
  - `htsim/sim/sing_cc.cpp`
- 已接入：
  - `Nscc::initCcParams(GlobalNetworkParams, FlowBasicParams, CcProfile, init_cwnd)`
  - `Dctcp::initCcParams(GlobalNetworkParams, FlowBasicParams, CcProfile, init_cwnd)`
  - `Swift::initCcParams(GlobalNetworkParams, FlowBasicParams, CcProfile, init_cwnd)`
  - `Constant::initCcParams(GlobalNetworkParams, FlowBasicParams, CcProfile, init_cwnd)`
- 目的：参数归属回到 CC 类内部，`SingSrc` 不再维护 NSCC 私有派生参数副本。

实现项 G：清理旧 CLI 与入口职责
- 位置：`htsim/sim/datacenter/main_sing.cpp`
- 改动：
  - 删除 `-target_q_delay`、`-qa_gate` 命令行参数与 usage 文本。
  - `initCcGlobalDefaults(...)` 仅传入网络全局事实（rtt/bdp/linkspeed/trimming）。
- 结果：NSCC 的 `_target_Qdelay`、`qa_gate`、`min/max_cwnd` 默认推导只在 `Nscc::initCcParams(...)` 内发生。

实现项 H：接入 `cc_profile` 文件解析（首版）
- 新增：`htsim/sim/sing_cc_profile.h`、`htsim/sim/sing_cc_profile.cpp`
- 功能：
  - 读取 `cc.defaults` 与 `cc.profiles`；
  - `params` 支持常量、布尔、表达式（`{\"expr\":\"...\"}`）；
  - 提供统一解析与覆盖决策：`parseAlgo/resolve`（支持 flow 级优先级规则）。
- 调用路径（当前实现）：
  - `main_sing.cpp` 通过 `-cc_profile <file>` 加载 `CcProfileStore`；
  - `connection_matrix.cpp` 解析 `.cm` 的 `cc_algo/cc_profile`；
  - `main_sing.cpp` 对每个 flow 调用 `CcProfileFlowSelectionResolver::resolve(...)`；
  - `SingSrc::setFlowCcSelection(...)` 注入生效算法与 profile；
  - `initCcForFlow/createPathwiseSubflows` 用该 flow 的 effective 选择创建 CC。

### 3.3 构建验证
- 命令：`cmake --build /tmp/htsim-sing-check --target htsim_sing --parallel`
- 结果：通过（仅存在历史 warning，无新增编译错误）。
