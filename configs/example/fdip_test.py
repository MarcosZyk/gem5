#!/usr/bin/env python3

# Copyright (c) 2023 The Regents of The University of Michigan
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
Test script for Fetch-Directed Instruction Prefetching
"""

import sys
import os

import m5
from m5.objects import *

# Create a simple system with an O3 CPU
system = System(cpu_clk_domain=SrcClockDomain(clock="2GHz"),
                mem_clk_domain=SrcClockDomain(clock="1GHz"),
                mem_ranges=[AddrRange("512MB")],
                cache_line_size=64)

# Create an out-of-order CPU
system.cpu = DerivO3CPU()

# Create a memory bus
system.membus = SystemXBar()

# Connect the CPU ports to the membus
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.mmio_port = system.membus.cpu_side_ports

# Create a memory controller and connect it to the membus
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# Create L1 caches
system.cpu.icache = L1_ICache(size="32kB", assoc=4)
system.cpu.dcache = L1_DCache(size="32kB", assoc=4)

# Connect the CPU ports to the L1 caches
system.cpu.icache_port = system.cpu.icache.cpu_side
system.cpu.dcache_port = system.cpu.dcache.cpu_side

# Create an L2 cache and connect the L1 caches to it
system.l2cache = L2Cache(size="256kB", assoc=8)
system.cpu.icache.mem_side = system.l2cache.cpu_side
system.cpu.dcache.mem_side = system.l2cache.cpu_side

# Connect the L2 cache to the memory bus
system.l2cache.mem_side = system.membus.cpu_side_ports

# Create the Fetch-Directed Instruction Prefetcher
system.cpu.icache.prefetcher = FetchDirectedPrefetcher()
system.cpu.icache.prefetcher.degree = 4
system.cpu.icache.prefetcher.max_streams = 16
system.cpu.icache.prefetcher.cross_pages = False

# Connect the prefetcher to the CPU's fetch unit
system.cpu.fetch.setInstPrefetcher(system.cpu.icache.prefetcher)

# Create a process to run
process = Process()
process.cmd = ['tests/test-progs/hello/bin/x86/linux/hello']
system.cpu.workload = process
system.cpu.createThreads()

# Set up the root SimObject and start the simulation
root = Root(full_system=False, system=system)

# Instantiate the simulation
m5.instantiate()

# Print a message and begin simulation
print("Beginning simulation!")
exit_event = m5.simulate()
print('Exiting @ tick {} because {}'
      .format(m5.curTick(), exit_event.getCause()))