#!/usr/bin/env python3
"""
Traffic Generator for htsim
Adapted from ns3 traffic generator to Python3 and htsim .cm format

Usage:
    python3 traffic_gen.py -c WebSearch_distribution.txt -n 256 -l 0.3 -b 400G -t 0.1
    
    Generates traffic according to the specified flow size distribution,
    for N hosts, at specified network load with given host bandwidth for T seconds.
"""

import sys
import random
import math
import heapq
from optparse import OptionParser
from custom_rand import CustomRand

class Flow:
    def __init__(self, src, dst, size, t, flowid):
        self.src, self.dst, self.size, self.t, self.flowid = src, dst, size, t, flowid
    
    def __str__(self):
        # htsim format: src->dst id flowid start start_time size flow_size
        # Convert time from seconds to microseconds
        t_us = self.t * 1e6
        return "%d->%d id %d start %.9f size %d" % (self.src, self.dst, self.flowid, t_us, self.size)

def translate_bandwidth(b):
    if b == None:
        return None
    if type(b) != str:
        return None
    if b[-1] == 'G':
        return float(b[:-1]) * 1e9
    if b[-1] == 'M':
        return float(b[:-1]) * 1e6
    if b[-1] == 'K':
        return float(b[:-1]) * 1e3
    return float(b)

def poisson(lam):
    return -math.log(1 - random.random()) * lam

if __name__ == "__main__":
    port = 80
    parser = OptionParser()
    parser.add_option("-c", "--cdf", dest="cdf_file", 
                      help="the file of the traffic size cdf", 
                      default="uniform_distribution.txt")
    parser.add_option("-n", "--nhost", dest="nhost", 
                      help="number of hosts")
    parser.add_option("-l", "--load", dest="load", 
                      help="the percentage of the traffic load to the network capacity, by default 0.3", 
                      default="0.3")
    parser.add_option("-b", "--bandwidth", dest="bandwidth", 
                      help="the bandwidth of host link (G/M/K), by default 10G", 
                      default="10G")
    parser.add_option("-t", "--time", dest="time", 
                      help="the total run time (s), by default 10", 
                      default="10")
    parser.add_option("-o", "--output", dest="output", 
                      help="the output file", 
                      default="tmp_traffic.cm")
    parser.add_option("-s", "--seed", dest="seed",
                      help="random seed (default: None - use system time)",
                      default=None)
    
    options, args = parser.parse_args()
    
    # Set random seed if provided
    if options.seed is not None:
        random.seed(int(options.seed))
    
    base_t = 0.0  # Start time in seconds (htsim uses seconds)
    
    if not options.nhost:
        print("please use -n to enter number of hosts")
        sys.exit(0)
    
    nhost = int(options.nhost)
    load = float(options.load)
    bandwidth = translate_bandwidth(options.bandwidth)
    time = float(options.time)  # Keep in seconds for htsim
    output = options.output
    
    if bandwidth == None:
        print("bandwidth format incorrect")
        sys.exit(0)
    
    fileName = options.cdf_file
    file = open(fileName, "r")
    lines = file.readlines()
    
    # read the cdf, save in cdf as [[x_i, cdf_i] ...]
    cdf = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):  # Skip empty lines and comments
            continue
        parts = line.split()
        if len(parts) >= 2:
            x, y = float(parts[0]), float(parts[1])
            cdf.append([x, y])
    
    # create a custom random generator, which takes a cdf, and generate number according to the cdf
    customRand = CustomRand()
    if not customRand.setCdf(cdf):
        print("Error: Not valid cdf")
        sys.exit(0)
    
    ofile = open(output, "w")
    
    # generate flows
    avg = customRand.getAvg()
    avg_inter_arrival = 1 / (bandwidth * load / 8.0 / avg)  # in seconds
    n_flow_estimate = int(time / avg_inter_arrival * nhost)
    n_flow = 0
    
    # Write header (will update Connections count later)
    ofile.write("Nodes %d\n" % nhost)
    ofile.write("Connections %d\n" % n_flow_estimate)
    
    # Use a heap to track next flow start time for each host
    host_list = [(base_t + poisson(avg_inter_arrival), i) for i in range(nhost)]
    heapq.heapify(host_list)
    
    flows = []
    flowid = 1
    
    while len(host_list) > 0:
        t, src = host_list[0]
        inter_t = poisson(avg_inter_arrival)
        
        dst = random.randint(0, nhost - 1)
        while (dst == src):
            dst = random.randint(0, nhost - 1)
        
        if (t + inter_t > time + base_t):
            heapq.heappop(host_list)
        else:
            size = int(customRand.rand())
            if size <= 0:
                size = 1
            
            flow = Flow(src, dst, size, t, flowid)
            flows.append(flow)
            flowid += 1
            n_flow += 1
            
            heapq.heapreplace(host_list, (t + inter_t, src))
    
    # Sort flows by start time
    flows.sort(key=lambda x: x.t)
    
    # Write flows
    for f in flows:
        ofile.write("%s\n" % str(f))
    
    # Go back and update the Connections count with actual number
    ofile.seek(0)
    ofile.write("Nodes %d\n" % nhost)
    ofile.write("Connections %d" % n_flow)
    
    ofile.close()
    
    print("Generated %d flows for %d hosts" % (n_flow, nhost))
    print("Average flow size: %.2f bytes" % avg)
    print("Average inter-arrival time: %.9f seconds (%.2f us)" % (avg_inter_arrival, avg_inter_arrival * 1e6))
    print("Simulation time: %.2f seconds" % time)
    print("Output written to: %s (start times in microseconds)" % output)

