#!/usr/bin/env python3
"""
Traffic Generator for htsim
Adapted from ns3 traffic generator to Python3 and htsim .cm format

Usage:
    python3 traffic_gen_with_incast.py -c WebSearch_distribution.txt -n 256 -l 0.3 -b 400G -t 0.01
    
    Generates traffic according to the specified flow size distribution,
    for N hosts, at specified network load with given host bandwidth for T seconds.

    Optionally, you can also generate incast traffic alongside the background traffic.
    Incast generation is enabled when all of the following are provided:
      - --incast_scale > 0
      - --incast_message_size > 0
      - --incast_load > 0
      - --concurrent_incast_num >= 1 (default: 1)
    
    python3 traffic_gen_with_incast.py -c WebSearch_distribution.txt -n 256 -l 0.4 -b 400G -t 0.01 \
        --incast_scale 16 --incast_message_size 1000000 --incast_load 0.3 --concurrent_incast_num 4
    
    python3 traffic_gen_with_incast.py -c FbHdp2015.txt -n 256 -l 0.4 -b 400G -t 0.01 \
        --incast_scale 16 --incast_message_size 1000000 --incast_load 0.5 --concurrent_incast_num 4
"""

import sys
import random
import math
import heapq
from optparse import OptionParser
from custom_rand import CustomRand

class Flow:
    def __init__(self, src, dst, size, t, flowid, flow_type=""):
        self.src, self.dst, self.size, self.t, self.flowid = src, dst, size, t, flowid
        self.flow_type = flow_type
    
    def __str__(self):
        # htsim format: src->dst id <flowid> start <us> size <bytes> [type <string>]
        # Convert time from seconds to microseconds.
        t_us = self.t * 1e6
        parts = [
            "%d->%d" % (self.src, self.dst),
            "id", str(self.flowid),
            "start", "%.9f" % t_us,
            "size", str(self.size),
        ]
        if self.flow_type:
            parts += ["type", self.flow_type]
        return " ".join(parts)

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


def _sample_sources_excluding_dst(nhost: int, dst: int, k: int):
    """Sample k unique sources from [0, nhost) excluding dst."""
    if k <= 0:
        return []
    if k > nhost - 1:
        raise ValueError("incast_scale must be <= nhost-1")

    # Sample from a compact range [0, nhost-1) then map values >= dst by +1.
    # This avoids allocating a full list of candidates.
    choices = random.sample(range(nhost - 1), k)
    return [c + 1 if c >= dst else c for c in choices]


def _sample_incast_receivers(nhost: int, concurrent: int):
    """Pick `concurrent` receivers for one incast burst.

    If concurrent <= nhost, receivers are unique; otherwise, allow repeats.
    """
    if concurrent <= 0:
        return []
    if concurrent <= nhost:
        return random.sample(range(nhost), concurrent)
    return [random.randint(0, nhost - 1) for _ in range(concurrent)]

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

    parser.add_option("--incast_scale", dest="incast_scale",
                      help="incast fan-in (number of senders per incast event); enable when >0",
                      default="0")
    parser.add_option("--incast_message_size", dest="incast_message_size",
                      help="incast flow size in bytes (per sender); enable when >0",
                      default="0")
    parser.add_option("--incast_load", dest="incast_load",
                      help="incast offered load as a fraction of total host capacity (0-1); enable when >0",
                      default="0")

    parser.add_option("--concurrent_incast_num", dest="concurrent_incast_num",
                      help="number of incast events launched simultaneously per incast burst (default: 1)",
                      default="1")
    
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

    incast_scale = int(options.incast_scale)
    incast_message_size = int(options.incast_message_size)
    incast_load = float(options.incast_load)
    concurrent_incast_num = int(options.concurrent_incast_num)
    
    if bandwidth == None:
        print("bandwidth format incorrect")
        sys.exit(0)

    incast_enabled = (incast_scale > 0 and incast_message_size > 0 and incast_load > 0)
    if not incast_enabled:
        if incast_scale != 0 or incast_message_size != 0 or incast_load != 0:
            print("Warning: incast params incomplete; incast traffic disabled")
    else:
        if incast_scale >= nhost:
            print("Error: --incast_scale must be <= nhost-1")
            sys.exit(0)
        if incast_load <= 0 or incast_load > 1:
            print("Error: --incast_load must be in (0, 1]")
            sys.exit(0)
        if concurrent_incast_num <= 0:
            print("Error: --concurrent_incast_num must be >= 1")
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
    
    # generate flows
    avg = customRand.getAvg()
    avg_inter_arrival = 1 / (bandwidth * load / 8.0 / avg)  # in seconds
    n_flow = 0

    # Incast: generate bursts at a rate derived from incast_load.
    # Each burst launches `concurrent_incast_num` incast events simultaneously.
    # Each incast event consists of `incast_scale` concurrent flows of size incast_message_size.
    # Total offered load (bytes/s) is: (bw * nhost * incast_load) / 8.
    if incast_enabled:
        burst_bits = concurrent_incast_num * incast_scale * incast_message_size * 8.0
        incast_avg_inter_arrival = burst_bits / (bandwidth * nhost * incast_load)
    else:
        incast_avg_inter_arrival = None
    
    # Use a heap to track next event per host (normal traffic) plus an optional incast event stream.
    # Heap item format: (time_s, node, kind) where kind=0 normal, kind=1 incast.
    event_heap = [(base_t + poisson(avg_inter_arrival), i, 0) for i in range(nhost)]
    if incast_enabled:
        event_heap.append((base_t + poisson(incast_avg_inter_arrival), random.randint(0, nhost - 1), 1))
    heapq.heapify(event_heap)
    
    flows = []
    flowid = 1

    while len(event_heap) > 0:
        t, node, kind = event_heap[0]

        if kind == 0:
            src = node
            inter_t = poisson(avg_inter_arrival)

            dst = random.randint(0, nhost - 1)
            while dst == src:
                dst = random.randint(0, nhost - 1)

            if t + inter_t > time + base_t:
                heapq.heappop(event_heap)
                continue

            size = int(customRand.rand())
            if size <= 0:
                size = 1

            flows.append(Flow(src, dst, size, t, flowid))
            flowid += 1
            n_flow += 1

            heapq.heapreplace(event_heap, (t + inter_t, src, 0))

        else:
            # Incast burst: launch multiple incast events at the same timestamp.
            # For each event, pick a receiver and incast_scale unique senders.
            dsts = _sample_incast_receivers(nhost, concurrent_incast_num)
            inter_t = poisson(incast_avg_inter_arrival)

            if t + inter_t > time + base_t:
                heapq.heappop(event_heap)
                continue

            for dst in dsts:
                try:
                    srcs = _sample_sources_excluding_dst(nhost, dst, incast_scale)
                except ValueError as e:
                    print("Error:", str(e))
                    sys.exit(0)

                for src in srcs:
                    flows.append(Flow(src, dst, incast_message_size, t, flowid, flow_type="incast"))
                    flowid += 1
                n_flow += incast_scale

            # Keep a comparable int in heap even if it's not used.
            heapq.heapreplace(event_heap, (t + inter_t, dsts[0] if dsts else 0, 1))
    
    # Sort flows by start time
    flows.sort(key=lambda x: x.t)
    
    with open(output, "w") as ofile:
        ofile.write("Nodes %d\n" % nhost)
        ofile.write("Connections %d\n" % n_flow)
        for f in flows:
            ofile.write("%s\n" % str(f))
    
    print("Generated %d flows for %d hosts" % (n_flow, nhost))
    print("Average flow size: %.2f bytes" % avg)
    print("Average inter-arrival time: %.9f seconds (%.2f us)" % (avg_inter_arrival, avg_inter_arrival * 1e6))
    if incast_enabled:
        print(
            "Incast enabled: scale=%d, message_size=%d bytes, load=%.4f, concurrent=%d"
            % (incast_scale, incast_message_size, incast_load, concurrent_incast_num)
        )
        print("Incast avg inter-arrival: %.9f seconds (%.2f us)" % (incast_avg_inter_arrival, incast_avg_inter_arrival * 1e6))
    print("Simulation time: %.2f seconds" % time)
    print("Output written to: %s (start times in microseconds)" % output)

