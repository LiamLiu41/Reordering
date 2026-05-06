# 流量生成器 (Traffic Generator)

这是一个 Python3 版本的流量生成器，用于为 htsim 仿真器生成流量矩阵文件（.cm 格式）。

原始代码来自 ns3 仿真器，已改造为：
- Python3 兼容
- 输出 htsim 的 .cm 文件格式

## 使用方法

### 基本用法

```bash
python3 traffic_gen.py -h  # 查看帮助
```

### 示例

```bash
python3 traffic_gen.py -c WebSearch_distribution.txt -n 320 -l 0.3 -b 400G -t 0.1
```

这个命令会：
- 根据 WebSearch 流大小分布生成流量
- 为 320 个主机生成流量
- 网络负载为 30%
- 主机带宽为 100Gbps
- 仿真时间为 0.1 秒

### 参数说明

- `-c, --cdf`: CDF 文件路径（流大小分布）
- `-n, --nhost`: 主机数量（必需）
- `-l, --load`: 网络负载百分比（默认 0.3，即 30%）
- `-b, --bandwidth`: 主机链路带宽（支持 G/M/K 后缀，默认 10G）
- `-t, --time`: 仿真运行时间（秒，默认 10）
- `-o, --output`: 输出文件路径（默认 tmp_traffic.cm）
- `-s, --seed`: 随机数种子（可选，用于可重复实验）

### 完整示例

```bash
# 使用 WebSearch 分布，生成高负载流量
python3 traffic_gen.py \
    -c WebSearch_distribution.txt \
    -n 128 \
    -l 0.8 \
    -b 40G \
    -t 1.0 \
    -o my_workload.cm \
    -s 42
```

## CDF 文件格式

CDF（累积分布函数）文件用于指定流大小的分布。格式为：

```
<流大小(字节)> <百分位数(0-100)>
<流大小(字节)> <百分位数(0-100)>
...
```

### 要求

1. 第一行的百分位数必须是 0
2. 最后一行的百分位数必须是 100
3. 流大小和百分位数必须严格递增

### 示例：uniform_distribution.txt

```
100 0
100000 100
```

这表示流大小在 100 字节到 100,000 字节之间均匀分布。

### 示例：WebSearch_distribution.txt

```
6 0
6 50
13 60
19 70
33 80
53 90
133 95
667 99
1333 99.5
3333 99.9
6667 100
```

这个分布模拟了 Web 搜索流量的特征（大量小流，少量大流）。

## 输出格式

生成的 .cm 文件格式：

```
Nodes <主机数量>
Connections <连接数量>
<src>-><dst> id <flowid> start <start_time> size <flow_size>
<src>-><dst> id <flowid> start <start_time> size <flow_size>
...
```

### 示例输出

```
Nodes 16
Connections 100
0->5 id 1 start 0.000123456 size 64000
3->7 id 2 start 0.000234567 size 128000
...
```

## 流量生成算法

流量生成器使用以下方法：

1. **泊松到达过程**：每个主机的流到达间隔服从泊松分布
2. **随机目的地**：目的主机随机选择（但不会选择自己）
3. **CDF 采样**：流大小根据提供的 CDF 分布随机采样
4. **负载控制**：通过调整平均流间隔时间来控制网络负载

平均流间隔时间计算公式：
```
avg_inter_arrival = 1 / (bandwidth * load / 8 / avg_flow_size)
```

## 常用分布文件

在 `connection_matrices/general_workload/` 目录下有一些常用的工作负载分布：

- `GoogleRPC2008.txt`: Google 数据中心 RPC 流量
- `FbHdp2015.txt`: Facebook Hadoop 流量
- `AliStorage2019.txt`: 阿里云存储流量
- `Solar2022.txt`: Solar 工作负载

## 注意事项

1. **主机数量**：必须与拓扑中的主机数量匹配
2. **流 ID**：自动递增，从 1 开始
3. **时间单位**：输出文件中的时间单位是秒
4. **流大小单位**：字节（Bytes）
5. **可重复性**：使用 `-s` 参数设置随机种子可以生成可重复的流量

## Python 依赖

只需要 Python 3 标准库，无需额外安装包。

