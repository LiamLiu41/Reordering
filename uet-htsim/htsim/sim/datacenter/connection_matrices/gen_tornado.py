#!/usr/bin/env python3
# python3 gen_tornado.py torn_8192n_8192c_64MB.cm 8192 8192 64000000 0 0
# Generate a tornado traffic matrix.
# python gen_tornado.py <filename> <nodes> <conns> <flowsize> <extrastarttime> <randseed>
# Parameters:
# <filename>    output filename for the connection matrix
# <nodes>       number of nodes in the topology (must be even)
# <conns>       number of active connections
# <flowsize>    size of the flows in bytes
# <extrastarttime>   How long in microseconds to space the start times over (start time will be random in between 0 and this time). Can be a float.
# <randseed>    Seed for random number generator, or set to 0 for random seed

# Tornado Pattern:
# Each node sends to its "twin" node in the other half of the tree.
# For N nodes: node i sends to node (i + N/2) % N
# This creates worst-case load balancing as packets traverse the full tree.

import os
import sys
from random import seed, random

def print_usage():
    print("Usage: python gen_tornado.py <filename> <nodes> <conns> <flowsize> <extrastarttime> <randseed>")
    print("  filename:        output connection matrix file")
    print("  nodes:           number of nodes (must be even)")
    print("  conns:           number of connections to generate")
    print("  flowsize:        size of each flow in bytes")
    print("  extrastarttime:  time window for random start times (microseconds)")
    print("  randseed:        random seed (0 for random)")
    print()
    print("Tornado Pattern:")
    print("  Each node i sends to node (i + nodes/2) % nodes")
    print("  This creates worst-case load balancing scenario")

def generate_tornado_pairs(nodes):
    """
    生成tornado流量模式的源-目标对
    每个节点与另一半中的孪生节点通信
    """
    if nodes % 2 != 0:
        raise ValueError("Tornado pattern requires an even number of nodes")
    
    pairs = []
    half = nodes // 2
    
    for i in range(nodes):
        # 计算孪生节点：另一半的对应位置
        twin = (i + half) % nodes
        pairs.append((i, twin))
    
    return pairs

def main():
    if len(sys.argv) != 7:
        print_usage()
        sys.exit(1)
    
    try:
        filename = sys.argv[1]
        nodes = int(sys.argv[2])
        conns = int(sys.argv[3])
        flowsize = int(sys.argv[4])
        extrastarttime = float(sys.argv[5])
        randseed = int(sys.argv[6])
    except ValueError as e:
        print(f"❌ 参数解析错误: {e}")
        print_usage()
        sys.exit(1)
    
    # 验证参数
    if nodes <= 0:
        print("❌ 错误: 节点数必须为正数")
        sys.exit(1)
    
    if nodes % 2 != 0:
        print("❌ 错误: Tornado模式要求节点数为偶数")
        sys.exit(1)
    
    if conns <= 0:
        print("❌ 错误: 连接数必须为正数")
        sys.exit(1)
    
    if conns > nodes:
        print("❌ 错误: 连接数不能超过节点数")
        sys.exit(1)
    
    if flowsize <= 0:
        print("❌ 错误: 流大小必须为正数")
        sys.exit(1)
    
    if extrastarttime < 0:
        print("❌ 错误: 额外开始时间不能为负数")
        sys.exit(1)
    
    print("🌪️  生成Tornado流量矩阵")
    print("=" * 50)
    print(f"📄 输出文件: {filename}")
    print(f"🖥️  节点数: {nodes}")
    print(f"🔗 连接数: {conns}")
    print(f"📦 流大小: {flowsize:,} bytes ({flowsize/(1024*1024):.1f} MB)")
    print(f"⏰ 开始时间窗口: {extrastarttime} us")
    print(f"🎲 随机种子: {randseed if randseed != 0 else 'random'}")
    print()
    
    # 设置随机种子
    if randseed != 0:
        seed(randseed)
    
    # 生成tornado配对
    try:
        tornado_pairs = generate_tornado_pairs(nodes)
    except ValueError as e:
        print(f"❌ 错误: {e}")
        sys.exit(1)
    
    print("🌪️  Tornado配对模式:")
    print("   节点 -> 孪生节点")
    for i in range(min(8, nodes)):  # 只显示前8个配对作为示例
        src, dst = tornado_pairs[i]
        print(f"   {src:3d} -> {dst:3d}")
    if nodes > 8:
        print(f"   ... (共{nodes}个配对)")
    print()
    
    # 写入连接矩阵文件
    try:
        with open(filename, "w") as f:
            f.write(f"Nodes {nodes}\n")
            f.write(f"Connections {conns}\n")
            
            # 生成指定数量的连接
            selected_pairs = tornado_pairs[:conns]
            
            for conn_id, (src, dst) in enumerate(selected_pairs):
                # 生成随机开始时间（微秒）
                start_time_us = random() * extrastarttime if extrastarttime > 0 else 0
                start_time_ps = int(start_time_us * 1000000)  # 转换为皮秒
                
                # 写入连接行（完全按照原始.cm文件格式）
                connection_line = f"{src}->{dst} start {start_time_ps} size {flowsize}"
                f.write(connection_line + "\n")
                
                # 显示前几个连接作为验证
                if conn_id < 5:
                    print(f"   连接 {conn_id + 1}: {src} -> {dst}, 开始时间: {start_time_us:.1f}us, 大小: {flowsize:,}B")
            
            if conns > 5:
                print(f"   ... (共生成{conns}个连接)")
    
    except IOError as e:
        print(f"❌ 文件写入错误: {e}")
        sys.exit(1)
    
    print()
    print("✅ Tornado流量矩阵生成完成!")
    print(f"📄 文件已保存: {filename}")
    
    # 生成统计信息
    unique_srcs = len(set(pair[0] for pair in selected_pairs))
    unique_dsts = len(set(pair[1] for pair in selected_pairs))
    
    print()
    print("📊 统计信息:")
    print(f"   总连接数: {conns}")
    print(f"   唯一源节点: {unique_srcs}")
    print(f"   唯一目标节点: {unique_dsts}")
    print(f"   流量对称性: {'是' if conns == nodes else '否'}")
    
    if conns == nodes:
        print("   🎯 完整Tornado模式: 每个节点都参与通信")
    else:
        print(f"   📝 部分Tornado模式: 使用前{conns}个配对")

if __name__ == "__main__":
    main()