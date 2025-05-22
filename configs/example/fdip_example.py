#!/usr/bin/env python3

# Copyright (c) 2024 All rights reserved
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.

"""
This script demonstrates how to use the Fetch Directed Instruction Prefetching (FDIP)
in gem5. It compares FDIP with next-line prefetching.
"""

import argparse
import sys
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

def get_processes(options):
    """Interprets provided options and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = options.cmd.split(';')
    if options.input != "":
        inputs = options.input.split(';')
    if options.output != "":
        outputs = options.output.split(';')
    if options.errout != "":
        errouts = options.errout.split(';')
    if options.options != "":
        pargs = options.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process(pid = 100 + idx)
        process.executable = wrkld
        process.cwd = os.getcwd()

        if options.env:
            with open(options.env, 'r') as f:
                process.env = [line.rstrip() for line in f]

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        multiprocesses.append(process)
        idx += 1

    if options.smt:
        assert(options.cpu_type == "DerivO3CPU")
        return multiprocesses
    else:
        return multiprocesses[0]

parser = argparse.ArgumentParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

parser.add_argument("--prefetcher", type=str, default="fdip",
                    choices=["fdip", "stride", "none"],
                    help="Type of prefetcher to use")
parser.add_argument("--l2_size", type=str, default="2MB",
                    help="L2 cache size")
parser.add_argument("--l2_assoc", type=int, default=8,
                    help="L2 cache associativity")
parser.add_argument("--prefetch-degree", type=int, default=2,
                    help="Prefetch degree (number of blocks to prefetch)")
parser.add_argument("--max-lookahead", type=int, default=16,
                    help="Maximum number of instructions to look ahead for FDIP")
parser.add_argument("--confidence-threshold", type=int, default=50,
                    help="Minimum confidence threshold for FDIP (0-100)")
parser.add_argument("--max-streams", type=int, default=16,
                    help="Maximum number of branch streams to track")
parser.add_argument("--dedicated-predictor", action="store_true",
                    help="Use a dedicated branch predictor for FDIP")

options = parser.parse_args()

multiprocesses = []
numThreads = 1

if options.bench:
    apps = options.bench.split("-")
    if len(apps) != options.num_cpus:
        print("number of benchmarks not equal to set num_cpus!")
        sys.exit(1)

    for app in apps:
        try:
            if buildEnv['TARGET_ISA'] == 'arm':
                exec("workload = %s('arm_%s', 'linux', '%s')" % (
                        app, options.arm_iset, options.spec_input))
            else:
                exec("workload = %s(buildEnv['TARGET_ISA', 'linux', '%s')" % (
                        app, options.spec_input))
            multiprocesses.append(workload.makeProcess())
        except:
            print("Unable to find workload for %s: %s" %
                  (buildEnv['TARGET_ISA'], app),
                  file=sys.stderr)
            sys.exit(1)
elif options.cmd:
    multiprocesses = get_processes(options)
else:
    print("No workload specified. Exiting!\n", file=sys.stderr)
    sys.exit(1)

# Create the system
system = System(cpu = [DerivO3CPU(cpu_id=0)],
                mem_mode = 'timing',
                mem_ranges = [AddrRange(options.mem_size)],
                cache_line_size = 64)

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage = options.sys_voltage)

# Create a source clock for the system and set the clock period
system.clk_domain = SrcClockDomain(clock = options.sys_clock,
                                   voltage_domain = system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a source clock for the CPUs and set the clock period
system.cpu_clk_domain = SrcClockDomain(clock = options.cpu_clock,
                                       voltage_domain = system.cpu_voltage_domain)

# All CPUs belong to a common CPU clock domain
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain

# Setup the workload
if isinstance(multiprocesses, list):
    for cpu, workload in zip(system.cpu, multiprocesses):
        cpu.workload = workload
        cpu.createThreads()
else:
    system.cpu[0].workload = multiprocesses
    system.cpu[0].createThreads()

# Create the L1 instruction and data caches
for cpu in system.cpu:
    # Create an L1 instruction and data cache
    cpu.icache = L1_ICache(size='32kB', assoc=2)
    cpu.dcache = L1_DCache(size='32kB', assoc=2)
    
    # Connect the instruction and data caches to the CPU
    cpu.icache.connectCPU(cpu)
    cpu.dcache.connectCPU(cpu)

# Create an L2 cache and connect it to the L1 caches
system.l2bus = L2XBar()
for cpu in system.cpu:
    cpu.icache.connectBus(system.l2bus)
    cpu.dcache.connectBus(system.l2bus)

# Create an L2 cache
system.l2cache = L2Cache(size=options.l2_size, assoc=options.l2_assoc)
system.l2cache.connectCPUSideBus(system.l2bus)

# Configure the prefetcher
if options.prefetcher == "fdip":
    # Create TAGE parameters if using a dedicated predictor
    if options.dedicated_predictor:
        tage_params = TAGE()
        tournament_params = TournamentBP()
    else:
        tage_params = TAGE()
        tournament_params = TournamentBP()
    
    # Use FDIP prefetcher in the L1 instruction cache
    for cpu in system.cpu:
        # Add FDIP prefetcher to each CPU's L1 instruction cache
        cpu.icache.prefetcher = FDIPrefetcher(
            degree=options.prefetch_degree,
            max_lookahead=options.max_lookahead,
            use_tage=True,
            thread_id=0,
            confidence_threshold=options.confidence_threshold,
            max_streams=options.max_streams,
            create_dedicated_predictor=options.dedicated_predictor,
            tage_params=tage_params,
            tournament_params=tournament_params
        )
    
    # If not using a dedicated predictor, connect to the CPU's branch predictor
    if not options.dedicated_predictor:
        # We need to connect the branch predictor to the prefetcher after the system is instantiated
        # This will be done in a callback function
        def connectBranchPredictor(root):
            for cpu in root.system.cpu:
                if hasattr(cpu.icache, 'prefetcher'):
                    cpu.icache.prefetcher.setBranchPredictor(cpu.getBranchPredictor())
        
        # Register the callback to be called after instantiation
        m5.registerCallback(connectBranchPredictor)
    
    # Register callbacks for branch misprediction notifications
    def notifyBranchMisprediction(pc, target, confidence=0, cpu_num=0):
        """Notify the prefetcher about a branch misprediction"""
        if cpu_num < len(system.cpu) and hasattr(system.cpu[cpu_num].icache, 'prefetcher'):
            system.cpu[cpu_num].icache.prefetcher.notifyBranchMisprediction(pc, target, confidence)
    
    def notifyCorrectPrediction(pc, target, confidence=0, cpu_num=0):
        """Notify the prefetcher about a correct branch prediction"""
        if cpu_num < len(system.cpu) and hasattr(system.cpu[cpu_num].icache, 'prefetcher'):
            system.cpu[cpu_num].icache.prefetcher.notifyCorrectPrediction(pc, target, confidence)
    
    # Hook these callbacks into the CPU's branch predictor
    for cpu in system.cpu:
        cpu.branchPred.mispredictHandler = notifyBranchMisprediction
        cpu.branchPred.correctPredictHandler = notifyCorrectPrediction
    
elif options.prefetcher == "stride":
    # Use stride prefetcher in the L1 instruction cache
    for cpu in system.cpu:
        cpu.icache.prefetcher = StridePrefetcher(degree=options.prefetch_degree)
else:
    # No prefetcher
    for cpu in system.cpu:
        cpu.icache.prefetcher = NULL

# Create a memory bus
system.membus = SystemXBar()
system.l2cache.connectMemSideBus(system.membus)

# Connect the system up to the membus
system.system_port = system.membus.cpu_side_ports

# Create a memory controller and connect it to the membus
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# Set up the system
root = Root(full_system = False, system = system)

# Instantiate the system
m5.instantiate()

# Simulate until program terminates
print("Beginning simulation!")
exit_event = m5.simulate()
print('Exiting @ tick {} because {}'
      .format(m5.curTick(), exit_event.getCause()))