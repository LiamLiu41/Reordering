# 实验运行器使用说明

## 概述

实验运行器 (`exp_runner.py`) 是一个用于批量运行实验的工具，支持：
- 多阶段（stage）实验管理
- 每个stage内的实验并行执行
- 自动CPU亲和性设置
- 实时监控和状态汇报
- 灵活的参数配置

## JSON配置文件格式

### 基本结构

```json
{
  "stages": [
    {
      "name": "stage名称",
      "description": "stage描述（可选）",
      "base_command": "基础命令",
      "common_args": {
        "公共参数名": "参数值",
        "标志参数": null
      },
      "experiments": [
        {
          "name": "实验名称",
          "args": {
            "实验特定参数": "值"
          },
          "stdout": "标准输出文件路径"
        }
      ]
    }
  ]
}
```

### 字段说明

#### Stage级别
- **name**: Stage的名称（必填）
- **description**: Stage的描述（可选）
- **base_command**: 基础命令，如 `./htsim_uec`（必填）
- **common_args**: 该stage内所有实验共享的参数（可选）
- **experiments**: 该stage要运行的实验列表（必填）

#### Experiment级别
- **name**: 实验名称，用于标识和汇报（必填）
- **args**: 实验特定参数，会覆盖common_args中的同名参数（必填）
- **stdout**: 标准输出重定向文件路径（必填）

#### 参数格式
- 普通参数：`"-param": "value"` → 生成 `-param value`
- 标志参数：`"-flag": null` → 生成 `-flag`（无值）
- 实验参数会覆盖公共参数

### 示例配置

```json
{
  "stages": [
    {
      "name": "mtu_experiments",
      "base_command": "./htsim_uec",
      "common_args": {
        "-tm": "connection_matrices/perm_1024n_1024c_0u_2000000b.cm",
        "-nodes": "1024",
        "-sender_cc_only": null,
        "-end": "5000"
      },
      "experiments": [
        {
          "name": "mtu_4KB",
          "args": {
            "-mtu": "4096",
            "-o": "./results/log_4KB.dat"
          },
          "stdout": "./results/log_4KB.out"
        },
        {
          "name": "mtu_2KB",
          "args": {
            "-mtu": "2048",
            "-o": "./results/log_2KB.dat"
          },
          "stdout": "./results/log_2KB.out"
        }
      ]
    }
  ]
}
```

## 使用方法

### 基本用法

```bash
# 在htsim/sim/datacenter目录下运行
cd /home/xiangzhou/uet-htsim/htsim/sim/datacenter

# 使用默认配置文件
python exp_runner.py

# 或直接执行
./exp_runner.py
```

### 指定配置文件

```bash
python exp_runner.py -c exp_config/my_config.json
```

### 指定工作目录

```bash
python exp_runner.py -c config.json -w /path/to/working/directory
```

### 指定起始CPU

```bash
# 从CPU 4开始分配
python exp_runner.py --cpu 4
```

### 保存结果到文件

```bash
python exp_runner.py -o results.json
```

### 完整示例

```bash
cd htsim/sim/datacenter
python3 exp_runner.py -c exp_config/template.json -o exp_config/experiment_results.json
```

## 命令行参数

```
-c, --config    JSON配置文件路径 (默认: exp_config/template.json)
-w, --workdir   工作目录 (默认: 配置文件所在目录的父目录)
--cpu           起始CPU ID (默认: 0)
-o, --output    保存结果的JSON文件路径
-h, --help      显示帮助信息
```

## 执行流程

1. **加载配置**: 读取JSON配置文件
2. **按Stage顺序执行**: 
   - Stage 0完成后才开始Stage 1
   - Stage 1完成后才开始Stage 2
   - 以此类推
3. **Stage内并行执行**:
   - 同一stage内的所有实验同时启动
   - 每个实验分配独立的CPU（通过taskset）
   - 每个实验在独立的进程中运行
4. **监控和汇报**:
   - 实时显示每个实验的启动和完成状态
   - 显示实验名称、执行时间、返回码
   - 每个stage完成后显示统计信息
5. **生成报告**:
   - 所有stage完成后显示总结
   - 可选保存详细结果到JSON文件

## 输出示例

```
================================================================================
🎯 实验运行器启动
   配置文件: exp_config/template.json
   工作目录: /home/xiangzhou/uet-htsim/htsim/sim/datacenter
   Stages数量: 2
================================================================================

================================================================================
📊 Stage: stage0_mtu_experiments
   描述: MTU size experiments
   实验数量: 3
================================================================================

🚀 启动实验: mtu_4KB
   CPU: 0
   命令: ./htsim_uec -tm connection_matrices/... -mtu 4096 ...
   输出: ./exp_results/mtu_exp/log_4KB.out

🚀 启动实验: mtu_2KB
   CPU: 1
   ...

✅ 实验完成: mtu_4KB (耗时: 45.23秒)
✅ 实验完成: mtu_2KB (耗时: 43.18秒)

================================================================================
📈 Stage 1/2 完成: stage0_mtu_experiments
   成功: 3, 失败: 0, 异常: 0
   耗时: 46.35秒
================================================================================

...

================================================================================
📋 实验运行总结
================================================================================
总实验数: 5
成功: 5
失败: 0
异常: 0
总耗时: 123.45秒
================================================================================

详细结果:
实验名称                        状态         耗时(秒)      返回码    
--------------------------------------------------------------------------------
mtu_4KB                        ✅ success      45.23     0         
mtu_2KB                        ✅ success      43.18     0         
...
```

## 目录结构

实验运行器会自动创建输出目录，建议的目录结构：

```
htsim/sim/datacenter/
├── exp_runner.py           # 运行器脚本
├── exp_config/             # 配置文件目录
│   ├── template.json       # 模板配置
│   └── my_exp.json         # 自定义配置
├── exp_results/            # 实验结果目录（自动创建）
│   ├── mtu_exp/
│   │   ├── log_4KB.dat
│   │   ├── log_4KB.out
│   │   └── ...
│   └── linkspeed_exp/
│       └── ...
└── htsim_uec              # 可执行文件
```

## 注意事项

1. **CPU亲和性**: 需要足够的CPU核心，否则taskset可能失败
2. **输出目录**: 会自动创建，确保有写权限
3. **工作目录**: 命令在指定的工作目录下执行，确保路径正确
4. **Stage依赖**: Stage按顺序执行，前一个完成才开始下一个
5. **失败处理**: 如果某个实验失败，会显示警告但继续执行

## 高级用法

### 创建自己的配置文件

1. 复制`template.json`为新文件
2. 修改stage名称和描述
3. 设置base_command和common_args
4. 添加experiments列表
5. 为每个实验设置name、args和stdout

### 参数覆盖示例

```json
{
  "common_args": {
    "-mtu": "4096",      // 公共值：4096
    "-nodes": "1024"
  },
  "experiments": [
    {
      "name": "exp1",
      "args": {
        "-mtu": "2048"   // 覆盖：使用2048
      }
    }
  ]
}
```

### 组织复杂实验

对于复杂实验，建议：
1. 将不同类型的实验分到不同的stage
2. 使用描述字段记录实验目的
3. 使用有意义的实验名称
4. 保持输出文件的组织结构清晰

## 故障排查

### 问题：实验失败
- 检查stdout文件中的错误信息
- 确认命令在工作目录下可以手动执行
- 检查所有路径是否正确

### 问题：找不到文件
- 确认工作目录设置正确
- 使用绝对路径或相对于工作目录的路径

### 问题：CPU亲和性失败
- 确保系统有足够的CPU核心
- 可以移除taskset功能（修改代码）

## 获取帮助

```bash
python exp_runner.py --help
```

