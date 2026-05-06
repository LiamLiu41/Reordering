#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Queue Length Analysis Script - Plot queue length over time for specified queues
"""

import os
import re
import argparse
import matplotlib.pyplot as plt
from matplotlib import rcParams
import numpy as np
from collections import defaultdict

# 设置matplotlib支持中文显示
rcParams['font.sans-serif'] = ['DejaVu Sans', 'SimHei', 'Arial Unicode MS']
rcParams['axes.unicode_minus'] = False

def parse_interest_file(interest_file):
    """
    解析interest.txt文件，获取感兴趣的队列名称和对应的plot_id
    
    Args:
        interest_file: interest.txt文件路径
        
    Returns:
        dict: {queue_name: plot_id}
    """
    interest_queues = {}
    
    if not os.path.exists(interest_file):
        raise FileNotFoundError(f"Interest file not found: {interest_file}")
    
    print(f"Reading interest file: {interest_file}")
    with open(interest_file, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split()
            if len(parts) < 2:
                print(f"Warning: Invalid format at line {line_num}: {line}")
                continue
            
            # 队列名称可能包含空格前的所有内容，最后一个是plot_id
            plot_id = parts[-1]
            queue_name = ' '.join(parts[:-1])
            
            interest_queues[queue_name] = plot_id
            print(f"  Queue: {queue_name} -> Plot ID: {plot_id}")
    
    print(f"Total interest queues: {len(interest_queues)}")
    return interest_queues


def extract_queue_ids_from_data(log_data_file, interest_queues):
    """
    从log_data文件中提取队列名称到队列ID的映射
    
    Args:
        log_data_file: log .dat文件路径
        interest_queues: 感兴趣的队列字典 {queue_name: plot_id}
        
    Returns:
        dict: {queue_id: (queue_name, plot_id)}
    """
    queue_id_map = {}
    found_queues = set()
    
    if not os.path.exists(log_data_file):
        raise FileNotFoundError(f"Log data file not found: {log_data_file}")
    
    print(f"\nReading queue ID mappings from: {log_data_file}")
    
    # 正则表达式匹配格式: ": SRC0->LS0(0)=199"
    pattern = re.compile(r'^:\s+(.+?)=(\d+)$')
    
    with open(log_data_file, 'r', errors='ignore') as f:
        for line in f:
            line = line.strip()
            
            # 遇到元数据注释行（以 # 开头）说明队列映射部分结束，停止读取
            if line.startswith('#'):
                print("  Reached metadata section, stopping queue ID extraction")
                break
            
            match = pattern.match(line)
            if match:
                queue_name = match.group(1)
                queue_id = match.group(2)
                
                # 检查是否是感兴趣的队列
                if queue_name in interest_queues:
                    plot_id = interest_queues[queue_name]
                    queue_id_map[queue_id] = (queue_name, plot_id)
                    found_queues.add(queue_name)
                    print(f"  Found: {queue_name} = ID {queue_id} (Plot ID: {plot_id})")
            
            # 如果找到了所有感兴趣的队列，可以提前结束
            if len(found_queues) == len(interest_queues):
                print("  Found all required queues, stopping early")
                break
    
    # 检查是否所有队列都找到了
    missing_queues = set(interest_queues.keys()) - found_queues
    if missing_queues:
        raise ValueError(f"Could not find queue IDs for: {missing_queues}")
    
    print(f"Successfully mapped all {len(queue_id_map)} interest queues")
    return queue_id_map


def extract_queue_length_data(log_txt_file, queue_id_map):
    """
    从log_txt文件中提取队列长度数据
    
    Args:
        log_txt_file: log .txt文件路径
        queue_id_map: 队列ID映射 {queue_id: (queue_name, plot_id)}
        
    Returns:
        dict: {plot_id: [(time, queue_length), ...]}
    """
    queue_data = defaultdict(list)
    
    if not os.path.exists(log_txt_file):
        raise FileNotFoundError(f"Log txt file not found: {log_txt_file}")
    
    print(f"\nReading queue length data from: {log_txt_file}")
    print("This may take a while for large files...")
    
    # 正则表达式匹配队列长度事件
    # 格式: 0.000010000 Type QUEUE_APPROX ID 229 Ev RANGE LastQ 4150 MinQ 0 MaxQ 8300 Name SRC5->LS0(0)
    pattern = re.compile(r'^([\d.]+)\s+Type\s+QUEUE_APPROX\s+ID\s+(\d+)\s+Ev\s+RANGE\s+LastQ\s+(\d+)')
    
    line_count = 0
    match_count = 0
    
    with open(log_txt_file, 'r') as f:
        for line in f:
            line_count += 1
            if line_count % 1000000 == 0:
                print(f"  Processed {line_count} lines, found {match_count} matches...")
            
            match = pattern.match(line)
            if match:
                time = float(match.group(1))
                queue_id = match.group(2)
                queue_length = int(match.group(3))
                
                # 检查是否是感兴趣的队列
                if queue_id in queue_id_map:
                    queue_name, plot_id = queue_id_map[queue_id]
                    queue_data[plot_id].append((time, queue_length))
                    match_count += 1
    
    print(f"Total lines processed: {line_count}")
    print(f"Total matching queue events: {match_count}")
    
    # 打印每个队列的数据点数量
    for plot_id in sorted(queue_data.keys(), key=lambda x: int(x) if x.isdigit() else x):
        print(f"  Plot ID {plot_id}: {len(queue_data[plot_id])} data points")
    
    return dict(queue_data)


def plot_queue_length(queue_data, output_path, max_time_us=None):
    """
    绘制队列长度随时间变化的曲线
    
    Args:
        queue_data: {plot_id: [(time, queue_length), ...]}
        output_path: 输出图片路径
        max_time_us: 最大绘制时间（微秒），None表示绘制所有时间
    """
    print(f"\nGenerating plot...")
    
    if max_time_us is not None:
        max_time_sec = max_time_us / 1e6
        print(f"Time limit: {max_time_us} us ({max_time_sec * 1000:.2f} ms)")
    else:
        max_time_sec = None
        print(f"Time limit: None (plotting all data)")
    
    num_queues = len(queue_data)
    print(f"Number of queues to plot: {num_queues}")
    
    # 创建大图
    fig, ax = plt.subplots(figsize=(16, 10))
    
    # 生成足够多的颜色和线型组合
    colors = plt.cm.tab20(np.linspace(0, 1, 20))
    additional_colors = plt.cm.tab20b(np.linspace(0, 1, 20))
    all_colors = np.vstack([colors, additional_colors])
    
    line_styles = ['-', '--', '-.', ':']
    
    # 按plot_id排序
    sorted_plot_ids = sorted(queue_data.keys(), key=lambda x: int(x) if x.isdigit() else x)
    
    for idx, plot_id in enumerate(sorted_plot_ids):
        data_points = queue_data[plot_id]
        
        if not data_points:
            continue
        
        # 按时间排序
        data_points.sort(key=lambda x: x[0])
        
        # 过滤时间范围
        if max_time_sec is not None:
            data_points = [(t, l) for t, l in data_points if t <= max_time_sec]
        
        if not data_points:
            print(f"  Warning: No data points for Queue {plot_id} within time limit")
            continue
        
        times = [p[0] for p in data_points]
        lengths = [p[1] for p in data_points]
        
        # 选择颜色和线型
        color = all_colors[idx % len(all_colors)]
        linestyle = line_styles[idx % len(line_styles)]
        
        # 绘制曲线，使用较细的线条以便显示更多曲线
        ax.plot(times, lengths, 
               color=color, 
               linestyle=linestyle,
               linewidth=1.2,
               label=f'Queue {plot_id}',
               alpha=0.8)
    
    # 根据时间范围选择合适的标签
    if max_time_sec is not None and max_time_sec < 0.01:  # < 10ms
        title_suffix = f' (First {max_time_us:.0f} us)'
    elif max_time_sec is not None and max_time_sec < 1:  # < 1s
        title_suffix = f' (First {max_time_sec*1000:.2f} ms)'
    elif max_time_sec is not None:
        title_suffix = f' (First {max_time_sec:.2f} s)'
    else:
        title_suffix = ''
    
    ax.set_xlabel('Time (s)', fontsize=12, fontweight='bold')
    ax.set_ylabel('Queue Length (Bytes)', fontsize=12, fontweight='bold')
    ax.set_title(f'Queue Length Over Time{title_suffix}', fontsize=14, fontweight='bold')
    
    # 设置横轴刻度，避免太密集
    ax.xaxis.set_major_locator(plt.MaxNLocator(10))
    
    # 网格
    ax.grid(True, alpha=0.3, linestyle='--')
    
    # 图例设置，使用多列以节省空间
    ncol = min(4, (num_queues + 7) // 8)  # 根据队列数量自动调整列数
    ax.legend(loc='best', fontsize=8, ncol=ncol, framealpha=0.9)
    
    plt.tight_layout()
    
    # 保存图片
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Figure saved to: {output_path}")
    
    plt.show()


def main():
    """Main function"""
    parser = argparse.ArgumentParser(description='Analyze and plot queue length over time')
    parser.add_argument('--log_data', required=True, help='Path to log .dat file')
    parser.add_argument('--log_txt', required=True, help='Path to log .txt file')
    parser.add_argument('--output_path', required=True, help='Path to output PNG file')
    parser.add_argument('--interest', default='interest.txt', help='Path to interest.txt file (default: interest.txt)')
    parser.add_argument('--time', type=float, default=None, help='Maximum time to plot in microseconds (us). Example: --time 2000 for 2ms')
    
    args = parser.parse_args()
    
    print("=" * 80)
    print("Queue Length Analysis")
    print("=" * 80)
    print(f"Interest file: {args.interest}")
    print(f"Log data file: {args.log_data}")
    print(f"Log txt file: {args.log_txt}")
    print(f"Output path: {args.output_path}")
    if args.time is not None:
        print(f"Time limit: {args.time} us ({args.time/1000:.2f} ms)")
    else:
        print(f"Time limit: None (plot all data)")
    print("=" * 80)
    
    try:
        # 1. 解析interest.txt文件
        interest_queues = parse_interest_file(args.interest)
        
        # 2. 从log_data中提取队列ID映射
        queue_id_map = extract_queue_ids_from_data(args.log_data, interest_queues)
        
        # 3. 从log_txt中提取队列长度数据
        queue_data = extract_queue_length_data(args.log_txt, queue_id_map)
        
        # 4. 绘制队列长度曲线
        plot_queue_length(queue_data, args.output_path, max_time_us=args.time)
        
        print("\nAnalysis completed successfully!")
        
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    return 0


if __name__ == '__main__':
    exit(main())

