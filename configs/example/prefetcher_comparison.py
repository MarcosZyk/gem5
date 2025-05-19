#!/usr/bin/env python3

# Copyright (c) 2025 All-Hands
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
This script compares the performance of different instruction prefetchers:
1. No prefetching (baseline)
2. Next-line prefetching
3. Streaming buffer prefetching
4. Fetch Directed Prefetching

It runs the same benchmark with each prefetcher and reports performance metrics.
"""

import argparse
import csv
import os
import sys

import m5
from m5.objects import *
from m5.stats import periodicStatDump
from m5.util import addToPath

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

def create_cpu_and_system(args, prefetcher_type=None, prefetcher_params=None):
    """Create a system with the specified prefetcher"""
    
    # Create the system
    system = System(cpu = [DerivO3CPU(cpu_id=0)],
                    mem_mode = 'timing',
                    mem_ranges = [AddrRange('512MB')],
                    cache_line_size = 64)

    # Create a process
    process = Process()
    process.cmd = [args.cmd] + args.options.split()
    system.cpu[0].workload = process
    system.cpu[0].createThreads()

    # Set up the system's memory system
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports
    
    # Create caches
    system.l1i = L1ICache(size=args.l1i_size)
    system.l1d = L1DCache(size=args.l1d_size)
    system.l2 = L2Cache(size=args.l2_size, assoc=args.l2_assoc)
    system.l1toL2bus = L2XBar()
    
    # Connect the L1 caches to the CPU
    system.cpu[0].icache_port = system.l1i.cpu_side
    system.cpu[0].dcache_port = system.l1d.cpu_side
    
    # Connect the L1 caches to the L2 bus
    system.l1i.mem_side = system.l1toL2bus.cpu_side_ports
    system.l1d.mem_side = system.l1toL2bus.cpu_side_ports
    
    # Configure the prefetcher based on the type
    if prefetcher_type:
        if prefetcher_type == "NextLinePrefetcher":
            system.l2.prefetcher = StridePrefetcher(
                degree=1,
                latency=1,
                queue_size=32,
                queue_squash=True,
                queue_filter=True,
                queue_size_opt="true",
                queue_filter_opt="true",
                tag_prefetch="false",
                use_master_id="true"
            )
        elif prefetcher_type == "StreamingBufferPrefetcher":
            system.l2.prefetcher = StreamPrefetcher(
                degree=4,
                distance=16,
                latency=1,
                queue_size=32,
                queue_squash=True,
                queue_filter=True,
                queue_size_opt="true",
                queue_filter_opt="true",
                tag_prefetch="false",
                use_master_id="true"
            )
        elif prefetcher_type == "FetchDirectedPrefetcher":
            system.l2.prefetcher = FetchDirectedPrefetcher(
                piq_size=prefetcher_params.get("piq_size", 16),
                ftq_size=prefetcher_params.get("ftq_size", 16),
                prefetch_buffer_size=prefetcher_params.get("prefetch_buffer_size", 32),
                prefetch_degree=prefetcher_params.get("prefetch_degree", 4),
                prefetch_distance=prefetcher_params.get("prefetch_distance", 2)
            )
    
    # Connect the L2 cache to the memory bus
    system.l2.cpu_side = system.l1toL2bus.mem_side_ports
    system.l2.mem_side = system.membus.cpu_side_ports
    
    # Setup the memory system
    MemConfig.config_mem(args, system)
    
    return system

def run_simulation(args, prefetcher_type=None, prefetcher_params=None):
    """Run a simulation with the specified prefetcher and return stats"""
    
    # Create the system with the specified prefetcher
    system = create_cpu_and_system(args, prefetcher_type, prefetcher_params)
    
    # Set up the root SimObject and start the simulation
    root = Root(full_system = False, system = system)
    
    # Instantiate the system
    m5.instantiate()
    
    # Setup periodic stat dump
    periodicStatDump(args.stat_period)
    
    # Run the simulation
    print(f"Beginning simulation with prefetcher: {prefetcher_type if prefetcher_type else 'None'}")
    exit_event = m5.simulate(args.max_insts)
    print(f'Exiting @ tick {m5.curTick()} because {exit_event.getCause()}')
    
    # Collect stats
    stats = {}
    stats['prefetcher'] = prefetcher_type if prefetcher_type else 'None'
    stats['sim_ticks'] = m5.curTick()
    stats['sim_seconds'] = m5.curTick() / 1e12
    stats['ipc'] = system.cpu[0].ipc.value
    stats['icache_miss_rate'] = system.l1i.overall_miss_rate.value
    stats['l2_miss_rate'] = system.l2.overall_miss_rate.value
    
    if prefetcher_type == "FetchDirectedPrefetcher":
        stats['pf_candidates_identified'] = system.l2.prefetcher.pfCandidatesIdentified.value
        stats['pf_candidates_filtered'] = system.l2.prefetcher.pfCandidatesFiltered.value
        stats['pf_issued_to_l2'] = system.l2.prefetcher.pfIssuedToL2.value
        stats['pf_buffer_hits'] = system.l2.prefetcher.pfBufferHits.value
    
    return stats

def main():
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser)
    Options.addSEOptions(parser)
    
    # Cache configuration options
    parser.add_argument("--l1i-size", type=str, default="32kB",
                      help="L1 instruction cache size")
    parser.add_argument("--l1d-size", type=str, default="32kB",
                      help="L1 data cache size")
    parser.add_argument("--l2-size", type=str, default="1MB",
                      help="L2 cache size")
    parser.add_argument("--l2-assoc", type=int, default=8,
                      help="L2 cache associativity")
    
    # Simulation options
    parser.add_argument("--max-insts", type=int, default=100000000,
                      help="Maximum number of instructions to simulate")
    parser.add_argument("--stat-period", type=int, default=10000000,
                      help="Period in ticks to dump statistics")
    
    # Fetch Directed Prefetcher specific options
    parser.add_argument("--piq-size", type=int, default=16,
                      help="Size of the Prefetch Instruction Queue")
    parser.add_argument("--ftq-size", type=int, default=16,
                      help="Size of the Fetch Target Queue")
    parser.add_argument("--prefetch-buffer-size", type=int, default=32,
                      help="Size of the prefetch buffer")
    parser.add_argument("--prefetch-degree", type=int, default=4,
                      help="Number of prefetches to issue per cycle")
    parser.add_argument("--prefetch-distance", type=int, default=2,
                      help="Prefetch distance in blocks")
    
    # Output options
    parser.add_argument("--output-dir", type=str, default="prefetcher_comparison_results",
                      help="Directory to store results")
    
    args = parser.parse_args()
    
    # Create output directory if it doesn't exist
    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)
    
    # Prefetcher configurations to test
    prefetcher_configs = [
        {"type": None, "params": {}},  # No prefetching (baseline)
        {"type": "NextLinePrefetcher", "params": {}},  # Next-line prefetching
        {"type": "StreamingBufferPrefetcher", "params": {}},  # Streaming buffer prefetching
        {"type": "FetchDirectedPrefetcher", "params": {  # Fetch Directed Prefetching
            "piq_size": args.piq_size,
            "ftq_size": args.ftq_size,
            "prefetch_buffer_size": args.prefetch_buffer_size,
            "prefetch_degree": args.prefetch_degree,
            "prefetch_distance": args.prefetch_distance
        }}
    ]
    
    # Run simulations for each prefetcher configuration
    results = []
    for config in prefetcher_configs:
        # Reset the simulation
        m5.reset()
        
        # Run the simulation with the current prefetcher
        stats = run_simulation(args, config["type"], config["params"])
        results.append(stats)
    
    # Write results to CSV
    csv_file = os.path.join(args.output_dir, "prefetcher_comparison.csv")
    with open(csv_file, 'w', newline='') as f:
        fieldnames = ['prefetcher', 'sim_ticks', 'sim_seconds', 'ipc', 
                     'icache_miss_rate', 'l2_miss_rate', 
                     'pf_candidates_identified', 'pf_candidates_filtered', 
                     'pf_issued_to_l2', 'pf_buffer_hits']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            writer.writerow(result)
    
    # Print summary
    print("\nPrefetcher Comparison Summary:")
    print("=" * 80)
    print(f"{'Prefetcher':<25} {'IPC':<10} {'I$ Miss Rate':<15} {'L2 Miss Rate':<15}")
    print("-" * 80)
    for result in results:
        print(f"{result['prefetcher']:<25} {result['ipc']:<10.4f} {result['icache_miss_rate']:<15.4f} {result['l2_miss_rate']:<15.4f}")
    print("=" * 80)
    print(f"Detailed results saved to {csv_file}")

if __name__ == "__m5_main__":
    main()