#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict

"""
python3 fct_analysis_general.py \
    -f /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_reps_gran/log_4KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_reps_gran/log_8KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_reps_gran/log_16KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_reps_gran/log_32KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_reps_gran/log_64KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1/lb_switch_ar/log_oblivious_ar.out \
    -l "reps_4KB" "reps_8KB" "reps_16KB" "reps_32KB" "reps_64KB" "AR" \
    -o fct_slowdown_comparison_reps \
    -t receiver

python3 fct_analysis_general.py \
    -f /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1_800g/lb_reps_gran/log_4KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1_800g/lb_reps_gran/log_8KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1_800g/lb_reps_gran/log_16KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1_800g/lb_reps_gran/log_32KB.out \
       /home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/general_workload/web7hdp1_800g/lb_reps_gran/log_64KB.out \
    -l "reps_4KB" "reps_8KB" "reps_16KB" "reps_32KB" "reps_64KB" \
    -o fct_slowdown_comparison_reps_800g \
    -t receiver
"""

def parse_log_file(log_file, fct_type='sender'):
    """
    解析log文件，提取slowdown和flow size
    参数:
        log_file: log文件路径
        fct_type: 'sender' 或 'receiver'
    返回: [(slowdown, bytes), ...]
    """
    data = []
    
    if fct_type == 'sender':
        # 解析 sender FCT: Flow.*?slowdown X.*?flowsize Y (排除 receiver_fct: 开头的行)
        pattern = r'^(?!receiver_fct:).*?Flow.*?slowdown\s+([\d.]+).*?flowsize\s+(\d+)'
    elif fct_type == 'receiver':
        # 解析 receiver FCT: receiver_fct:.*?slowdown X.*?flowsize Y
        pattern = r'receiver_fct:.*?slowdown\s+([\d.]+).*?flowsize\s+(\d+)'
    else:
        raise ValueError(f"不支持的 FCT 类型: {fct_type}。请使用 'sender' 或 'receiver'")
    
    with open(log_file, 'r') as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                slowdown = float(match.group(1))
                bytes_val = int(match.group(2))
                # 确保slowdown >= 1
                slowdown = max(slowdown, 1.0)
                data.append((slowdown, bytes_val))
    
    return data

def get_percentile(arr, p):
    """
    获取第p百分位数 (p在0-1之间)
    """
    if len(arr) == 0:
        return 0
    arr_sorted = sorted(arr)
    idx = int(len(arr_sorted) * p)
    idx = min(idx, len(arr_sorted) - 1)
    return arr_sorted[idx]

def process_data(data, step=5, target_percentiles=[50, 95, 99]):
    """
    处理数据，按flow size分桶，计算每个桶的统计信息
    参数:
        data: [(slowdown, bytes), ...] 原始数据
        step: 百分位桶的步长
        target_percentiles: 要计算的分位数列表，如 [50, 75, 99]
    返回: {
        'percentiles': [...],  # 百分位数（用于横坐标）
        'flow_sizes': [...],
        'p50': [...],
        'p75': [...],
        'p99': [...],
        ...
    }
    """
    # 按bytes排序
    data_sorted = sorted(data, key=lambda x: x[1])
    
    n = len(data_sorted)
    if n == 0:
        return None
    
    results = {
        'percentiles': [],  # 存储百分位数
        'flow_sizes': []
    }
    
    # 动态创建分位数字段
    for p in target_percentiles:
        results[f'p{p}'] = []
    
    # 分成多个桶
    for i in range(0, 100, step):
        left = i * n // 100
        right = (i + step) * n // 100
        
        if left >= right or right > n:
            continue
        
        bucket = data_sorted[left:right]
        slowdowns = [x[0] for x in bucket]
        flow_sizes_in_bucket = [x[1] for x in bucket]
        
        # 使用桶中flow size的中位数，更能代表这个百分位段
        flow_size = get_percentile(flow_sizes_in_bucket, 0.5)
        
        results['percentiles'].append(i + step / 2)  # 桶的中间百分位
        results['flow_sizes'].append(flow_size / 1024)  # 转换为KB
        
        # 动态计算各个分位数
        for p in target_percentiles:
            percentile_value = get_percentile(slowdowns, p / 100.0)
            results[f'p{p}'].append(percentile_value)
    
    return results

def format_flow_size(size_kb):
    """格式化flow size显示"""
    if size_kb < 1:
        bytes_val = size_kb * 1024
        if bytes_val < 1:
            return f"{bytes_val:.2f}B"
        else:
            return f"{int(bytes_val)}B"
    elif size_kb < 1024:
        # 对于小数KB，显示一位小数
        if size_kb < 10:
            return f"{size_kb:.1f}KB"
        else:
            return f"{int(size_kb)}KB"
    else:
        mb_val = size_kb / 1024
        if mb_val < 10:
            return f"{mb_val:.1f}MB"
        else:
            return f"{int(mb_val)}MB"

def plot_results(all_results, labels, output_prefix='fct_slowdown', target_percentiles=[50, 95, 99]):
    """
    绘制多张图：根据target_percentiles指定的分位数
    横坐标位置按百分位数等距，标签显示flow size
    参数:
        all_results: 所有结果的列表
        labels: 标签列表
        output_prefix: 输出文件名前缀
        target_percentiles: 要绘制的分位数列表，如 [50, 75, 99]
    """
    num_plots = len(target_percentiles)
    fig, axes = plt.subplots(1, num_plots, figsize=(6 * num_plots, 5))
    
    # 如果只有一个子图，axes不是数组，需要转换
    if num_plots == 1:
        axes = [axes]
    
    metrics = [f'p{p}' for p in target_percentiles]
    titles = [f'P{p} FCT Slowdown' for p in target_percentiles]
    
    # 定义颜色和标记样式
    colors = ['red', 'orange', 'black', 'blue', 'green', 'purple', 'brown', 'pink']
    markers = ['o', 's', '^', 'v', 'D', 'p', '*', 'h']
    
    # 使用第一个有效结果来设置X轴刻度
    reference_result = None
    for result in all_results:
        if result is not None:
            reference_result = result
            break
    
    for idx, (metric, title) in enumerate(zip(metrics, titles)):
        ax = axes[idx]
        
        for i, (result, label) in enumerate(zip(all_results, labels)):
            if result is None:
                continue
            
            # 检查该结果是否包含该metric
            if metric not in result:
                continue
            
            color = colors[i % len(colors)]
            marker = markers[i % len(markers)]
            
            # 使用百分位数作为实际坐标位置（等距）
            ax.plot(result['percentiles'], result[metric], 
                   label=label, 
                   marker=marker, 
                   color=color,
                   linewidth=2,
                   markersize=6,
                   markerfacecolor='white',
                   markeredgewidth=2)
        
        # 设置X轴刻度：位置是百分位数，标签是flow size
        if reference_result:
            # 选择合适的刻度位置（不要太密集）
            tick_indices = list(range(0, len(reference_result['percentiles']), max(1, len(reference_result['percentiles']) // 10)))
            if len(reference_result['percentiles']) - 1 not in tick_indices:
                tick_indices.append(len(reference_result['percentiles']) - 1)
            
            tick_positions = [reference_result['percentiles'][i] for i in tick_indices]
            tick_labels = [format_flow_size(reference_result['flow_sizes'][i]) for i in tick_indices]
            
            ax.set_xticks(tick_positions)
            ax.set_xticklabels(tick_labels, rotation=45, ha='right')
        
        ax.set_xlabel('Flow size (KB)', fontsize=12)
        ax.set_ylabel('FCT Slowdown', fontsize=12)
        ax.set_title(title, fontsize=14, fontweight='bold')
        ax.legend(fontsize=10)
        ax.grid(True, alpha=0.3)
        ax.set_xlim(0, 100)  # 百分位数范围 0-100
    
    plt.tight_layout()
    plt.savefig(f'{output_prefix}_all.png', dpi=300, bbox_inches='tight')
    print(f"已保存图片: {output_prefix}_all.png")
    
    # 分别保存单独的图
    for idx, (metric, title) in enumerate(zip(metrics, titles)):
        fig_single, ax = plt.subplots(figsize=(8, 6))
        
        for i, (result, label) in enumerate(zip(all_results, labels)):
            if result is None:
                continue
            
            # 检查该结果是否包含该metric
            if metric not in result:
                continue
            
            color = colors[i % len(colors)]
            marker = markers[i % len(markers)]
            
            # 使用百分位数作为实际坐标位置（等距）
            ax.plot(result['percentiles'], result[metric], 
                   label=label, 
                   marker=marker, 
                   color=color,
                   linewidth=2,
                   markersize=6,
                   markerfacecolor='white',
                   markeredgewidth=2)
        
        # 设置X轴刻度：位置是百分位数，标签是flow size
        if reference_result:
            # 选择合适的刻度位置
            tick_indices = list(range(0, len(reference_result['percentiles']), max(1, len(reference_result['percentiles']) // 10)))
            if len(reference_result['percentiles']) - 1 not in tick_indices:
                tick_indices.append(len(reference_result['percentiles']) - 1)
            
            tick_positions = [reference_result['percentiles'][i] for i in tick_indices]
            tick_labels = [format_flow_size(reference_result['flow_sizes'][i]) for i in tick_indices]
            
            ax.set_xticks(tick_positions)
            ax.set_xticklabels(tick_labels, rotation=45, ha='right')
        
        ax.set_xlabel('Flow size (KB)', fontsize=12)
        ax.set_ylabel('FCT Slowdown', fontsize=12)
        ax.set_title(title, fontsize=14, fontweight='bold')
        ax.legend(fontsize=10)
        ax.grid(True, alpha=0.3)
        ax.set_xlim(0, 100)  # 百分位数范围 0-100
        
        plt.tight_layout()
        plt.savefig(f'{output_prefix}_{metric}.png', dpi=300, bbox_inches='tight')
        print(f"已保存图片: {output_prefix}_{metric}.png")
        plt.close()
    
    plt.show()

def main():
    parser = argparse.ArgumentParser(description='FCT Slowdown分析和绘图工具')
    parser.add_argument('-f', '--files', nargs='+', required=True, 
                       help='输入的log文件路径（可以有多个）')
    parser.add_argument('-l', '--labels', nargs='+', required=True,
                       help='每个文件对应的标签名称')
    parser.add_argument('-t', '--type', choices=['sender', 'receiver'], default='sender',
                       help='FCT类型: sender (发送方) 或 receiver (接收方) (默认: sender)')
    parser.add_argument('-s', '--step', type=int, default=5,
                       help='百分位桶的步长 (默认: 5)')
    parser.add_argument('-p', '--percentiles', nargs='+', type=int, default=[50, 95, 99],
                       help='要绘制的分位数列表 (默认: 50 95 99，例如: -p 50 75 99)')
    parser.add_argument('-o', '--output', default='fct_slowdown',
                       help='输出图片文件名前缀 (默认: fct_slowdown)')
    
    args = parser.parse_args()
    
    if len(args.files) != len(args.labels):
        print("错误: 文件数量和标签数量必须一致!")
        return
    
    # 验证分位数范围
    for p in args.percentiles:
        if p < 0 or p > 100:
            print(f"错误: 分位数 {p} 超出范围 [0, 100]")
            return
    
    print(f"开始分析 {len(args.files)} 个文件...")
    print(f"FCT类型: {args.type}")
    print(f"要绘制的分位数: {args.percentiles}")
    
    all_results = []
    for log_file, label in zip(args.files, args.labels):
        print(f"\n处理文件: {log_file}")
        print(f"标签: {label}")
        
        # 解析数据
        data = parse_log_file(log_file, fct_type=args.type)
        print(f"  提取到 {len(data)} 条flow记录")
        
        if len(data) == 0:
            print(f"  警告: 文件 {log_file} 没有有效数据")
            all_results.append(None)
            continue
        
        # 处理数据
        result = process_data(data, step=args.step, target_percentiles=args.percentiles)
        all_results.append(result)
        
        if result:
            print(f"  Flow size 范围: {min(result['flow_sizes']):.2f} KB - {max(result['flow_sizes']):.2f} KB")
            print(f"  百分位数: {min(result['percentiles']):.1f}% - {max(result['percentiles']):.1f}%")
            for p in args.percentiles:
                print(f"  平均P{p} slowdown: {np.mean(result[f'p{p}']):.3f}")
    
    # 绘图
    print("\n开始绘图...")
    plot_results(all_results, args.labels, args.output, target_percentiles=args.percentiles)
    print("\n完成!")

if __name__ == "__main__":
    main()

