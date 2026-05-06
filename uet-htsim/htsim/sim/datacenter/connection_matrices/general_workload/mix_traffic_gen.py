#!/usr/bin/env python3
"""
Traffic Generator for htsim (Multi-Mix Version)

Usage:
    python3 mix_traffic_gen.py -m "FbHdp2015.txt:0.1,WebSearch.txt:0.7" -n 256 -b 400G -t 0.01
    python3 mix_traffic_gen.py -m "FbHdp2015.txt:0.1,WebSearch.txt:0.2,WebSearch.txt:0.2,WebSearch.txt:0.3" -n 256 -b 400G -t 0.01
    
    Generates mixed traffic types for N hosts.
    Each host runs independent Poisson processes for each traffic type.
"""

import sys
import random
import math
import heapq
import os
from optparse import OptionParser
# 假设 CustomRand 在同级目录下，或者你已经有了这个类
from custom_rand import CustomRand

class Flow:
    def __init__(self, src, dst, size, t, flowid, traffic_type):
        self.src = src
        self.dst = dst
        self.size = size
        self.t = t
        self.flowid = flowid
        self.traffic_type = traffic_type
    
    def __str__(self):
        # htsim format: src->dst id flowid start start_time size flow_size type traffic_type
        # Convert time from seconds to microseconds for output
        t_us = self.t * 1e6
        # 注意：这里在末尾追加了 type 字段，htsim 解析器可能需要对应修改，或者它会忽略多余字段
        return "%d->%d id %d start %.9f size %d type %s" % (self.src, self.dst, self.flowid, t_us, self.size, self.traffic_type)

def translate_bandwidth(b):
    if b == None: return None
    if type(b) != str: return None
    if b[-1] == 'G': return float(b[:-1]) * 1e9
    if b[-1] == 'M': return float(b[:-1]) * 1e6
    if b[-1] == 'K': return float(b[:-1]) * 1e3
    return float(b)

def poisson(lam):
    return -math.log(1 - random.random()) * lam

def load_cdf(filename):
    """Helper to load CDF file and return data points"""
    try:
        with open(filename, "r") as f:
            lines = f.readlines()
    except IOError:
        print(f"Error: Could not open file {filename}")
        sys.exit(1)
        
    cdf = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) >= 2:
            x, y = float(parts[0]), float(parts[1])
            cdf.append([x, y])
    return cdf

if __name__ == "__main__":
    parser = OptionParser()
    # 新增 -m 参数，格式为 "file1:load1,file2:load2"
    parser.add_option("-m", "--mix", dest="traffic_mix", 
                      help="Traffic mix config, format: 'file1.txt:0.3,file2.txt:0.4'", 
                      default=None)
    # 保留旧的 -c 和 -l 以兼容单一脸模式（可选）
    parser.add_option("-c", "--cdf", dest="cdf_file", help="single cdf file", default=None)
    parser.add_option("-l", "--load", dest="load", help="single load", default=None)
    
    parser.add_option("-n", "--nhost", dest="nhost", help="number of hosts")
    parser.add_option("-b", "--bandwidth", dest="bandwidth", help="bandwidth (G/M/K)", default="10G")
    parser.add_option("-t", "--time", dest="time", help="total run time (s)", default="10")
    parser.add_option("-o", "--output", dest="output", help="output file", default="tmp_traffic.cm")
    parser.add_option("-s", "--seed", dest="seed", help="random seed", default=None)
    
    options, args = parser.parse_args()
    
    if options.seed is not None:
        random.seed(int(options.seed))
    
    if not options.nhost:
        print("please use -n to enter number of hosts")
        sys.exit(0)

    nhost = int(options.nhost)
    bandwidth = translate_bandwidth(options.bandwidth)
    time_limit = float(options.time)
    base_t = 0.0
    
    # --- 解析流量混合配置 ---
    # 结构: list of dicts [{'cdf': data, 'load': 0.3, 'name': 'WebSearch', 'rand': obj, 'avg_int': 0.01}, ...]
    traffic_configs = []
    
    # 优先使用 -m 参数，如果没有则回退到 -c -l
    if options.traffic_mix:
        mix_entries = options.traffic_mix.split(',')
        for entry in mix_entries:
            parts = entry.split(':')
            if len(parts) != 2:
                print(f"Error: Invalid mix format '{entry}'. Use 'file:load'")
                sys.exit(1)
            fname = parts[0].strip()
            fload = float(parts[1])
            # 使用文件名（去掉扩展名）作为 Type 名称
            ftype = os.path.splitext(os.path.basename(fname))[0]
            traffic_configs.append({'file': fname, 'load': fload, 'type': ftype})
    elif options.cdf_file and options.load:
        fname = options.cdf_file
        fload = float(options.load)
        ftype = os.path.splitext(os.path.basename(fname))[0]
        traffic_configs.append({'file': fname, 'load': fload, 'type': ftype})
    else:
        print("Error: Must specify either -m or (-c and -l)")
        sys.exit(0)

    # --- 初始化每个流量类型的生成器 ---
    total_load = 0
    for config in traffic_configs:
        print(f"Initializing traffic type: {config['type']} (Load: {config['load']})")
        
        # 1. 加载 CDF
        cdf_data = load_cdf(config['file'])
        
        # 2. 初始化随机数生成器
        cr = CustomRand()
        if not cr.setCdf(cdf_data):
            print(f"Error: Invalid CDF in {config['file']}")
            sys.exit(0)
        config['rand_gen'] = cr
        
        # 3. 计算该类型的平均间隔
        avg_size = cr.getAvg()
        # 公式: Interval = 1 / (Bandwidth * Load_percentage / 8.0 / Avg_Packet_Size)
        # 注意：Load 是小数 (0.3)，Bandwidth 是 bps，Size 是 Bytes (需要 *8 转 bits)
        if config['load'] > 0:
            avg_inter_arrival = 1.0 / (bandwidth * config['load'] / 8.0 / avg_size)
        else:
            avg_inter_arrival = float('inf') # 负载为0，无限间隔
            
        config['avg_inter_arrival'] = avg_inter_arrival
        config['avg_size'] = avg_size
        total_load += config['load']

    print(f"Total Network Load: {total_load}")
    if total_load > 1.0:
        print("Warning: Total load exceeds 1.0, congestion is guaranteed.")

    # --- 初始化 Heap ---
    # Heap 元素结构: (time, src_host_id, config_index)
    # config_index 用于指代 traffic_configs 列表中的第几种流量
    event_heap = []
    
    for host_id in range(nhost):
        for idx, config in enumerate(traffic_configs):
            if config['avg_inter_arrival'] == float('inf'):
                continue
            # 为该主机的该类型流量生成第一个发包时间
            start_t = base_t + poisson(config['avg_inter_arrival'])
            heapq.heappush(event_heap, (start_t, host_id, idx))

    # --- 开始模拟 ---
    ofile = open(options.output, "w")
    
    # 预留 Header 位置
    ofile.write("Nodes %d\n" % nhost)
    # 这里的 Connections 数量是未知的，先写个占位符，最后 seek 回来修改
    header_pos = ofile.tell()
    ofile.write("Connections %-20d\n" % 0) # padding for later overwrite

    flows = []
    flowid = 1
    n_flow = 0
    
    while len(event_heap) > 0:
        # 1. 取出最早发生的事件
        t, src, config_idx = heapq.heappop(event_heap)
        
        # 2. 如果时间超限，停止处理该事件（且不再生成后续事件）
        if t > time_limit + base_t:
            continue
            
        # 3. 获取对应的配置
        config = traffic_configs[config_idx]
        
        # 4. 生成流信息
        dst = random.randint(0, nhost - 1)
        while dst == src:
            dst = random.randint(0, nhost - 1)
            
        size = int(config['rand_gen'].rand())
        if size <= 0: size = 1
        
        # 5. 记录流
        # 新增了 traffic_type 字段
        new_flow = Flow(src, dst, size, t, flowid, config['type'])
        flows.append(new_flow)
        
        flowid += 1
        n_flow += 1
        
        # 6. 调度该主机的该类型流量的下一次事件
        inter_t = poisson(config['avg_inter_arrival'])
        next_t = t + inter_t
        
        # 只有下一次时间还在范围内才 push，节省堆空间
        if next_t <= time_limit + base_t:
            heapq.heappush(event_heap, (next_t, src, config_idx))

    # --- 排序并写入 ---
    print("Sorting flows...")
    flows.sort(key=lambda x: x.t)
    
    print(f"Writing {len(flows)} flows to file...")
    for f in flows:
        ofile.write("%s\n" % str(f))
    
    # 更新 Header 中的 Connections 数量
    ofile.seek(0)
    ofile.write("Nodes %d\n" % nhost)
    ofile.write("Connections %d" % n_flow)
    
    ofile.close()
    print("Done.")