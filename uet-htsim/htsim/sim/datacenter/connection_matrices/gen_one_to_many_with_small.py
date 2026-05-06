#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Generate One-to-Many traffic matrix across ToRs with additional small flows.

Each node sends to all corresponding nodes in other ToRs within the same one-to-many group.
Additionally, small flows are generated at regular intervals with random cross-ToR endpoints.

Usage:
    python gen_one_to_many_with_small.py <filename> <nodes> <tor_size> <flowsize> <start_time_jitter> <small_flow_interval> <small_flow_size> <small_flow_num> <one_to_many_group_num> <randseed>

    python3 gen_one_to_many_with_small.py traffic.cm 256     16         8000000    20                  20                    8000              60               1                      42
    python3 gen_one_to_many_with_small.py traffic.cm 256     16         8000000    20                  10                    8000             120               1                      42
Parameters:
    <filename>                Output .cm file name
    <nodes>                   Total number of nodes in the topology
    <tor_size>                Number of nodes under each ToR switch
    <flowsize>                Size of one-to-many flows in bytes
    <start_time_jitter>       Random jitter range for one-to-many flow start times in microseconds (0 for no jitter)
    <small_flow_interval>     Time interval between small flows in microseconds
    <small_flow_size>         Size of small flows in bytes
    <small_flow_num>          Total number of small flows to generate
    <one_to_many_group_num>   Number of one-to-many groups (default 1 = all ToRs in one group)
    <randseed>                Seed for random number generator (0 for random seed)

Example:
    python3 gen_one_to_many_with_small.py m2n_case14.cm 256 16 4000000 20 6 8000 200 4 42
    
    This creates:
    - 256 nodes with 16 nodes per ToR (16 ToR switches total)
    - 2 one-to-many groups (8 ToRs per group)
    - One-to-many flows: each node sends to corresponding nodes in other ToRs within the same group
    - Small flows: 100 small flows of 64KB each, starting every 100us with random cross-ToR endpoints
    - All flows are cross-ToR by design
"""

import sys
from random import seed, uniform, randint, choice

def print_usage():
    print(__doc__)
    sys.exit(1)

if len(sys.argv) != 11:
    print("Error: Incorrect number of arguments")
    print_usage()

# Parse arguments
filename = sys.argv[1]
nodes = int(sys.argv[2])
tor_size = int(sys.argv[3])
flowsize = int(sys.argv[4])
start_time_jitter = float(sys.argv[5])  # in microseconds
small_flow_interval = float(sys.argv[6])  # in microseconds
small_flow_size = int(sys.argv[7])  # in bytes
small_flow_num = int(sys.argv[8])
one_to_many_group_num = int(sys.argv[9])
randseed = int(sys.argv[10])

# Validation
if nodes % tor_size != 0:
    print(f"Error: Total nodes ({nodes}) must be evenly divisible by tor_size ({tor_size})")
    sys.exit(1)

num_tors = nodes // tor_size

if num_tors % one_to_many_group_num != 0:
    print(f"Error: Number of ToRs ({num_tors}) must be evenly divisible by one_to_many_group_num ({one_to_many_group_num})")
    sys.exit(1)

tors_per_group = num_tors // one_to_many_group_num

# Print configuration
print("=" * 70)
print("One-to-Many + Small Flows Traffic Matrix Configuration")
print("=" * 70)
print(f"Total nodes:              {nodes}")
print(f"ToR size:                 {tor_size} nodes per ToR")
print(f"Number of ToRs:           {num_tors}")
print(f"One-to-many groups:       {one_to_many_group_num}")
print(f"ToRs per group:           {tors_per_group}")
print(f"\nOne-to-Many flows:")
print(f"  Flow size:              {flowsize} bytes")
print(f"  Start time jitter:      {start_time_jitter} us")
print(f"\nSmall flows:")
print(f"  Flow size:              {small_flow_size} bytes")
print(f"  Flow interval:          {small_flow_interval} us")
print(f"  Total number:           {small_flow_num}")
print(f"\nRandom seed:              {randseed}")
print("=" * 70)

# Initialize random seed (only used for jitter)
if randseed != 0:
    seed(randseed)

# Organize nodes by ToR (FIXED assignment)
# Node i is on ToR floor(i / tor_size)
print(f"\nFixed ToR assignment: {num_tors} ToRs with {tor_size} nodes each")
print(f"  ToR 0: nodes 0-{tor_size-1}")
print(f"  ToR 1: nodes {tor_size}-{2*tor_size-1}")
if num_tors > 2:
    print(f"  ...")
    print(f"  ToR {num_tors-1}: nodes {(num_tors-1)*tor_size}-{nodes-1}")

# Print one-to-many group assignment
print(f"\nOne-to-many group assignment:")
for group_id in range(one_to_many_group_num):
    start_tor = group_id * tors_per_group
    end_tor = start_tor + tors_per_group - 1
    print(f"  Group {group_id}: ToRs {start_tor}-{end_tor}")

# Helper function to get ToR ID for a node
def get_tor_id(node):
    return node // tor_size

# Helper function to get position within ToR (0 to tor_size-1)
def get_position_in_tor(node):
    return node % tor_size

# Helper function to get one-to-many group ID for a ToR
def get_one_to_many_group_id(tor_id):
    return tor_id // tors_per_group

# Calculate total connections
# Each node sends to (tors_per_group - 1) destinations (all other ToRs in same group)
one_to_many_conns = nodes * (tors_per_group - 1)
total_conns = one_to_many_conns + small_flow_num

# Open output file (will be rewritten later with correct count)
f = open(filename, "w")
print(f"Nodes {nodes}", file=f)
print(f"Connections {total_conns}", file=f)

flow_id = 1

# ============================================================
# Generate One-to-Many Traffic
# ============================================================
print("\n" + "=" * 70)
print("Generating One-to-Many Traffic")
print("=" * 70)
print("Communication pattern:")
print("  - Each node sends to all corresponding nodes in OTHER ToRs within the SAME one-to-many group")
print("  - Node i in ToR j → Node i in ToR k (for all k ≠ j in same group)")
print()

# For each source node
for src in range(nodes):
    src_tor = get_tor_id(src)
    src_pos = get_position_in_tor(src)
    src_group = get_one_to_many_group_id(src_tor)
    
    # Send to corresponding nodes in all other ToRs within the same group
    group_start_tor = src_group * tors_per_group
    group_end_tor = group_start_tor + tors_per_group
    
    for dst_tor in range(group_start_tor, group_end_tor):
        if dst_tor == src_tor:
            # Skip same ToR
            continue
        
        # Calculate destination node: same position in different ToR
        dst = dst_tor * tor_size + src_pos
        
        # Add random jitter to start time
        if start_time_jitter > 0:
            jitter = uniform(0, start_time_jitter)
            flow_start_time = jitter
        else:
            flow_start_time = 0.0
        
        print(f"{src}->{dst} id {flow_id} start {flow_start_time:.6f} size {flowsize} type one_to_many", file=f)
        flow_id += 1

# ============================================================
# Generate Small Flows
# ============================================================
print("\n" + "=" * 70)
print("Generating Small Flows")
print("=" * 70)
print(f"Generating {small_flow_num} small flows with random cross-ToR endpoints")
print(f"  - Flow size: {small_flow_size} bytes")
print(f"  - Interval: {small_flow_interval} us")
print()

small_flow_count = 0
for i in range(small_flow_num):
    # Calculate start time for this small flow
    flow_start_time = i * small_flow_interval
    
    # Randomly select source and destination ensuring they are on different ToRs
    max_attempts = 100
    attempt = 0
    while attempt < max_attempts:
        src = randint(0, nodes - 1)
        dst = randint(0, nodes - 1)
        
        src_tor = get_tor_id(src)
        dst_tor = get_tor_id(dst)
        
        # Ensure cross-ToR and src != dst
        if src_tor != dst_tor and src != dst:
            break
        attempt += 1
    
    if attempt >= max_attempts:
        print(f"Warning: Could not find cross-ToR pair after {max_attempts} attempts for small flow {i}")
        continue
    
    print(f"{src}->{dst} id {flow_id} start {flow_start_time:.6f} size {small_flow_size} type small", file=f)
    flow_id += 1
    small_flow_count += 1

f.close()

print(f"Successfully generated {small_flow_count} small flows")

# Print examples of the communication pattern
print("Example flows:")

# Show examples for first group
group_0_start_tor = 0
group_0_end_tor = tors_per_group

print(f"Group 0 (ToRs {group_0_start_tor}-{group_0_end_tor-1}):")
print(f"  Node 0 (ToR 0, pos 0) → nodes ", end="")
destinations = [tor_id * tor_size for tor_id in range(1, min(group_0_end_tor, 5))]
print(", ".join(str(d) for d in destinations), end="")
if tors_per_group > 4:
    print(f", ... ({tors_per_group - 1} destinations total)")
else:
    print(f" ({tors_per_group - 1} destinations total)")

print(f"  Node 1 (ToR 0, pos 1) → nodes ", end="")
destinations = [tor_id * tor_size + 1 for tor_id in range(1, min(group_0_end_tor, 5))]
print(", ".join(str(d) for d in destinations), end="")
if tors_per_group > 4:
    print(f", ... ({tors_per_group - 1} destinations total)")
else:
    print(f" ({tors_per_group - 1} destinations total)")

if one_to_many_group_num > 1:
    # Show examples for second group if it exists
    group_1_start_tor = tors_per_group
    group_1_end_tor = 2 * tors_per_group
    
    print(f"\nGroup 1 (ToRs {group_1_start_tor}-{group_1_end_tor-1}):")
    first_node_in_group_1 = group_1_start_tor * tor_size
    print(f"  Node {first_node_in_group_1} (ToR {group_1_start_tor}, pos 0) → nodes ", end="")
    destinations = [tor_id * tor_size for tor_id in range(group_1_start_tor + 1, min(group_1_end_tor, group_1_start_tor + 5))]
    print(", ".join(str(d) for d in destinations), end="")
    if tors_per_group > 4:
        print(f", ... ({tors_per_group - 1} destinations total)")
    else:
        print(f" ({tors_per_group - 1} destinations total)")

# ============================================================
# Summary
# ============================================================
actual_total_conns = one_to_many_conns + small_flow_count

# Rewrite file header with correct connection count
f = open(filename, "r")
lines = f.readlines()
f.close()

f = open(filename, "w")
f.write(f"Nodes {nodes}\n")
f.write(f"Connections {actual_total_conns}\n")
for line in lines[2:]:  # Skip first two header lines
    f.write(line)
f.close()

print("\n" + "=" * 70)
print("One-to-Many + Small Flows Traffic Matrix Generated Successfully!")
print("=" * 70)
print(f"Output file: {filename}")
print(f"Total flows generated: {actual_total_conns}")
print(f"\nOne-to-Many flows:")
print(f"  - Count: {one_to_many_conns}")
print(f"  - Size: {flowsize} bytes")
print(f"  - Groups: {one_to_many_group_num} groups, {tors_per_group} ToRs per group")
print(f"  - Pattern: Each node sends to {tors_per_group - 1} destinations (one in each other ToR within same group)")
if start_time_jitter > 0:
    print(f"  - Start time: 0 to {start_time_jitter} us (with jitter)")
else:
    print(f"  - Start time: 0 us (no jitter)")

print(f"\nSmall flows:")
print(f"  - Count: {small_flow_count}")
print(f"  - Size: {small_flow_size} bytes")
print(f"  - Interval: {small_flow_interval} us")
print(f"  - Pattern: Random cross-ToR endpoints")
if small_flow_num > 0:
    print(f"  - Start time range: 0 to {(small_flow_num-1) * small_flow_interval:.2f} us")

print(f"\n✓ All flows are cross-ToR by design")
print("=" * 70)

# Print example commands
print("\nExample simulation command:")
print(f"  ./main_uec -nodes {nodes} -tm {filename} -strat ecmp_host -end 10000 -o output.log")
print("\nAnalyze flows by type:")
print(f"  grep 'type one_to_many' output.log | wc -l")
print(f"    Expected: {one_to_many_conns} completed flows")
print(f"  grep 'type small' output.log | wc -l")
print(f"    Expected: {small_flow_count} completed flows")
print("=" * 70)