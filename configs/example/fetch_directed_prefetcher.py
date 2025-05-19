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
This script demonstrates the use of the Fetch Directed Prefetcher.
It configures a system with an O3 CPU and L1/L2 caches, with the
L2 cache using the Fetch Directed Prefetcher.
"""

import argparse
import sys

import m5
from m5.objects import *
from m5.util import addToPath

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

def main():
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser)
    Options.addSEOptions(parser)
    
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
    
    args = parser.parse_args()

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
    CacheConfig.config_cache(args, system)
    MemConfig.config_mem(args, system)
    
    # Configure the L2 cache to use the Fetch Directed Prefetcher
    system.l2 = L2Cache(size='1MB', assoc=8)
    system.l2.prefetcher = FetchDirectedPrefetcher(
        piq_size=args.piq_size,
        ftq_size=args.ftq_size,
        prefetch_buffer_size=args.prefetch_buffer_size,
        prefetch_degree=args.prefetch_degree,
        prefetch_distance=args.prefetch_distance
    )
    
    # Connect the L2 cache to the memory bus
    system.l2.cpu_side = system.l1toL2bus.mem_side_ports
    system.l2.mem_side = system.membus.cpu_side_ports

    # Set up the root SimObject and start the simulation
    root = Root(full_system = False, system = system)
    
    # Instantiate the system
    m5.instantiate()

    # Run the simulation
    print("Beginning simulation!")
    exit_event = m5.simulate()
    print('Exiting @ tick {} because {}'
          .format(m5.curTick(), exit_event.getCause()))

if __name__ == "__m5_main__":
    main()