#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FCT CDF Analysis by Flow Type - Plot FCT CDF curves for different methods
Separated by flow type
"""

import os
import re
import numpy as np
import matplotlib.pyplot as plt
from matplotlib import rcParams
from collections import defaultdict

# 设置matplotlib支持中文显示
rcParams['font.sans-serif'] = ['DejaVu Sans', 'SimHei', 'Arial Unicode MS']
rcParams['axes.unicode_minus'] = False

# ============ 配置部分 ============
# 指定要分析的方法及其日志路径
METHODS = {
    'bitmap_path_2': {
        'label': 'Bitmap Path=2',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_2.out'
    },
    'bitmap_path_4': {
        'label': 'Bitmap Path=4',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_4.out'
    },
    'bitmap_path_8': {
        'label': 'Bitmap Path=8',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_8.out'
    },
    'bitmap_path_16': {
        'label': 'Bitmap Path=16',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_16.out'
    },
    'bitmap_path_32': {
        'label': 'Bitmap Path=32',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_32.out'
    },
    'bitmap_path_64': {
        'label': 'Bitmap Path=64',
        'log_path': '/home/xiangzhou/uet-htsim/htsim/sim/datacenter/exp_results/try_alltoall_bitmap/log_path_64.out'
    }
}

# 指定要分析的 flow types
FLOW_TYPES = ['one_to_many', 'small']

# 输出目录
OUTPUT_DIR = '.'

# ============ 数据提取函数 ============
def extract_fct_by_type_from_log(log_file_path):
    """
    从日志文件中提取 Flow 完成记录，按 type 分类
    
    日志格式示例：
    Flow Uec_150_122 flowId 24938 uecSrc 24937 type m2n started at 500 
    finished at 595.221 duration 95.2212 us ideal_time 13.2043 us ...
    
    Args:
        log_file_path: 日志文件路径
        
    Returns:
        fct_by_type: 字典 {flow_type: [fct_list]}
    """
    fct_by_type = defaultdict(list)
    
    if not os.path.exists(log_file_path):
        print(f"Warning: File not found {log_file_path}")
        return fct_by_type
    
    print(f"Reading file: {log_file_path}")
    
    # 正则表达式匹配 Flow 行
    # 提取 type 和 duration
    pattern = re.compile(r'Flow.*type\s+(\S+).*duration\s+([\d.]+)\s+us')
    
    with open(log_file_path, 'r') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                flow_type = match.group(1)
                duration = float(match.group(2))
                fct_by_type[flow_type].append(duration)
    
    # 打印统计信息
    for flow_type, fcts in fct_by_type.items():
        print(f"  Found {len(fcts)} flows of type '{flow_type}'")
    
    return fct_by_type


def calculate_cdf(data):
    """
    计算CDF
    
    Args:
        data: 数据列表
        
    Returns:
        sorted_data: 排序后的数据
        cdf: 对应的CDF值
    """
    if len(data) == 0:
        return np.array([]), np.array([])
    
    sorted_data = np.sort(data)
    cdf = np.arange(1, len(sorted_data) + 1) / len(sorted_data)
    
    return sorted_data, cdf


# ============ 绘图函数 ============
def plot_fct_cdf_by_type(all_data):
    """
    为每个 flow type 绘制 FCT CDF 图
    
    Args:
        all_data: 嵌套字典 {method_key: {flow_type: [fct_list]}}
    """
    # 定义颜色和线型
    colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd', '#8c564b', '#e377c2', '#7f7f7f']
    linestyles = ['-', '--', '-.', ':', '-', '--', '-.', ':']
    
    for flow_type in FLOW_TYPES:
        print(f"\nPlotting CDF for flow type: {flow_type}")
        
        fig, ax = plt.subplots(figsize=(10, 7))
        
        method_idx = 0
        for method_key, method_info in METHODS.items():
            label = method_info['label']
            
            # 获取该方法的FCT数据
            if method_key not in all_data:
                print(f"  Warning: No data for method '{method_key}'")
                continue
            
            fct_list = all_data[method_key].get(flow_type, [])
            
            if len(fct_list) == 0:
                print(f"  Warning: No data for method '{label}' and flow type '{flow_type}'")
                continue
            
            # 计算CDF
            sorted_data, cdf = calculate_cdf(fct_list)
            
            # 绘制CDF曲线
            color = colors[method_idx % len(colors)]
            linestyle = linestyles[method_idx % len(linestyles)]
            
            ax.plot(sorted_data, cdf, 
                   color=color,
                   linestyle=linestyle,
                   linewidth=2,
                   label=f'{label} (n={len(fct_list)})')
            
            # 打印统计信息
            median = np.median(fct_list)
            p99 = np.percentile(fct_list, 99)
            print(f"  {label}: Count={len(fct_list)}, Median={median:.2f}us, P99={p99:.2f}us")
            
            method_idx += 1
        
        # 设置图表属性
        ax.set_xlabel('FCT (us)', fontsize=12, fontweight='bold')
        ax.set_ylabel('CDF', fontsize=12, fontweight='bold')
        ax.set_title(f'FCT CDF - {flow_type} flows', fontsize=14, fontweight='bold')
        ax.legend(loc='lower right', fontsize=10)
        ax.grid(True, alpha=0.3, linestyle='--')
        ax.set_ylim(0, 1.05)
        
        # 添加 P50 和 P99 参考线
        ax.axhline(y=0.5, color='gray', linestyle=':', alpha=0.5, label='P50')
        ax.axhline(y=0.99, color='gray', linestyle=':', alpha=0.5, label='P99')
        
        plt.tight_layout()
        
        # 保存图片
        output_file = os.path.join(OUTPUT_DIR, f'fct_cdf_{flow_type}.png')
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"  Figure saved to: {output_file}")
        
        plt.show()
        plt.close()


# ============ 主函数 ============
def main():
    """Main function"""
    print("=" * 60)
    print("FCT CDF Analysis (By Flow Type)")
    print("=" * 60)
    print(f"Methods: {', '.join([info['label'] for info in METHODS.values()])}")
    print(f"Flow Types: {', '.join(FLOW_TYPES)}")
    print("=" * 60)
    
    # 收集所有方法的数据：{method_key: {flow_type: [fct_list]}}
    all_data = {}
    
    for method_key, method_info in METHODS.items():
        label = method_info['label']
        log_path = method_info['log_path']
        
        print(f"\nProcessing method: {label}")
        print(f"  Log path: {log_path}")
        print("-" * 60)
        
        # 提取FCT数据（按类型分类）
        fct_by_type = extract_fct_by_type_from_log(log_path)
        all_data[method_key] = fct_by_type
    
    # 绘制CDF图
    print("\n" + "=" * 60)
    print("Generating CDF plots...")
    print("=" * 60)
    plot_fct_cdf_by_type(all_data)
    
    # 打印汇总统计
    print("\n" + "=" * 60)
    print("Summary Statistics")
    print("=" * 60)
    for flow_type in FLOW_TYPES:
        print(f"\n{flow_type.upper()} Flows:")
        print("-" * 50)
        print(f"{'Method':<15} {'Count':<10} {'Median (us)':<15} {'P99 (us)':<15}")
        print("-" * 50)
        
        for method_key, method_info in METHODS.items():
            label = method_info['label']
            fct_list = all_data.get(method_key, {}).get(flow_type, [])
            
            if len(fct_list) > 0:
                median = np.median(fct_list)
                p99 = np.percentile(fct_list, 99)
                print(f"{label:<15} {len(fct_list):<10} {median:<15.2f} {p99:<15.2f}")
            else:
                print(f"{label:<15} {'N/A':<10} {'N/A':<15} {'N/A':<15}")
    
    print("\nAnalysis completed!")


if __name__ == '__main__':
    main()

