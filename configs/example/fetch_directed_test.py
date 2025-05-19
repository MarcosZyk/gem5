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
This script tests specific features of the Fetch Directed Prefetcher:
1. Decoupled branch predictor and instruction cache
2. Marking fetch blocks kicked out of the instruction cache
3. Using idle instruction cache ports to filter prefetch requests

It runs a series of tests with different configurations to isolate and
measure the impact of each feature.
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

def create_cpu_and_system(args, test_config):
    """Create a system with the specified test configuration"""
    
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
    
    # Configure the Fetch Directed Prefetcher with specific test settings
    system.l2.prefetcher = FetchDirectedPrefetcher(
        piq_size=args.piq_size,
        ftq_size=args.ftq_size,
        prefetch_buffer_size=args.prefetch_buffer_size,
        prefetch_degree=args.prefetch_degree,
        prefetch_distance=args.prefetch_distance,
        # Test-specific configuration parameters
        enable_decoupled_bp=test_config.get("enable_decoupled_bp", True),
        enable_cache_eviction_tracking=test_config.get("enable_cache_eviction_tracking", True),
        enable_idle_port_filtering=test_config.get("enable_idle_port_filtering", True)
    )
    
    # Connect the L2 cache to the memory bus
    system.l2.cpu_side = system.l1toL2bus.mem_side_ports
    system.l2.mem_side = system.membus.cpu_side_ports
    
    # Setup the memory system
    MemConfig.config_mem(args, system)
    
    return system

def run_test(args, test_config):
    """Run a test with the specified configuration and return stats"""
    
    # Create the system with the specified test configuration
    system = create_cpu_and_system(args, test_config)
    
    # Set up the root SimObject and start the simulation
    root = Root(full_system = False, system = system)
    
    # Instantiate the system
    m5.instantiate()
    
    # Setup periodic stat dump
    periodicStatDump(args.stat_period)
    
    # Run the simulation
    print(f"Beginning simulation with test configuration: {test_config['name']}")
    exit_event = m5.simulate(args.max_insts)
    print(f'Exiting @ tick {m5.curTick()} because {exit_event.getCause()}')
    
    # Collect stats
    stats = {}
    stats['test_name'] = test_config['name']
    stats['sim_ticks'] = m5.curTick()
    stats['sim_seconds'] = m5.curTick() / 1e12
    stats['ipc'] = system.cpu[0].ipc.value
    stats['icache_miss_rate'] = system.l1i.overall_miss_rate.value
    stats['l2_miss_rate'] = system.l2.overall_miss_rate.value
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
    parser.add_argument("--output-dir", type=str, default="fetch_directed_test_results",
                      help="Directory to store results")
    
    args = parser.parse_args()
    
    # Create output directory if it doesn't exist
    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)
    
    # Define test configurations to isolate and measure each feature
    test_configs = [
        {
            "name": "baseline_no_prefetching",
            "enable_decoupled_bp": False,
            "enable_cache_eviction_tracking": False,
            "enable_idle_port_filtering": False
        },
        {
            "name": "decoupled_bp_only",
            "enable_decoupled_bp": True,
            "enable_cache_eviction_tracking": False,
            "enable_idle_port_filtering": False
        },
        {
            "name": "eviction_tracking_only",
            "enable_decoupled_bp": False,
            "enable_cache_eviction_tracking": True,
            "enable_idle_port_filtering": False
        },
        {
            "name": "idle_port_filtering_only",
            "enable_decoupled_bp": False,
            "enable_cache_eviction_tracking": False,
            "enable_idle_port_filtering": True
        },
        {
            "name": "decoupled_bp_and_eviction_tracking",
            "enable_decoupled_bp": True,
            "enable_cache_eviction_tracking": True,
            "enable_idle_port_filtering": False
        },
        {
            "name": "decoupled_bp_and_idle_port_filtering",
            "enable_decoupled_bp": True,
            "enable_cache_eviction_tracking": False,
            "enable_idle_port_filtering": True
        },
        {
            "name": "eviction_tracking_and_idle_port_filtering",
            "enable_decoupled_bp": False,
            "enable_cache_eviction_tracking": True,
            "enable_idle_port_filtering": True
        },
        {
            "name": "full_fetch_directed_prefetching",
            "enable_decoupled_bp": True,
            "enable_cache_eviction_tracking": True,
            "enable_idle_port_filtering": True
        }
    ]
    
    # Run tests for each configuration
    results = []
    for config in test_configs:
        # Reset the simulation
        m5.reset()
        
        # Run the test with the current configuration
        stats = run_test(args, config)
        results.append(stats)
    
    # Write results to CSV
    csv_file = os.path.join(args.output_dir, "fetch_directed_test_results.csv")
    with open(csv_file, 'w', newline='') as f:
        fieldnames = ['test_name', 'sim_ticks', 'sim_seconds', 'ipc', 
                     'icache_miss_rate', 'l2_miss_rate', 
                     'pf_candidates_identified', 'pf_candidates_filtered', 
                     'pf_issued_to_l2', 'pf_buffer_hits']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            writer.writerow(result)
    
    # Print summary
    print("\nFetch Directed Prefetcher Feature Test Summary:")
    print("=" * 100)
    print(f"{'Test Configuration':<40} {'IPC':<10} {'I$ Miss Rate':<15} {'L2 Miss Rate':<15} {'PF Buffer Hits':<15}")
    print("-" * 100)
    for result in results:
        print(f"{result['test_name']:<40} {result['ipc']:<10.4f} {result['icache_miss_rate']:<15.4f} {result['l2_miss_rate']:<15.4f} {result['pf_buffer_hits']:<15.0f}")
    print("=" * 100)
    print(f"Detailed results saved to {csv_file}")

if __name__ == "__m5_main__":
    main()