"""
scons build/X86/gem5.opt -j16

./build/X86/gem5.opt configs/fdip/fdip.py --prefetcher <prefetcher>
"""

import os
import argparse

from m5.objects import (
    LTAGE,
    TaggedPrefetcher,
    FetchDirectedInstructionPrefetcher,
    L2XBar,
)

from gem5.isas import ISA
from gem5.utils.requires import requires
from gem5.resources.resource import obtain_resource, BinaryResource
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.classic.caches.l1icache import L1ICache
from gem5.components.cachehierarchies.classic.caches.mmu_cache import MMUCache
from gem5.components.cachehierarchies.classic.caches.l1dcache import L1DCache
from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.components.processors.simple_core import SimpleCore
from gem5.simulate.simulator import Simulator


workloads = {
    "hello": "x86-hello64-static",
}

class CacheHierarchy(PrivateL1CacheHierarchy):
    def __init__(self, icache, dcache):
        super().__init__(l1i_size="", l1d_size="")
        self.icache = icache
        self.dcache = dcache

    def incorporate_cache(self, board: AbstractBoard) -> None:

        board.connect_system_port(self.membus.cpu_side_ports)

        for _, port in board.get_memory().get_mem_ports():
            self.membus.mem_side_ports = port

        cpu = board.get_processor().get_cores()[0]

        cpu.connect_icache(self.icache.cpu_side)
        self.icache.mem_side = self.membus.cpu_side_ports

        cpu.connect_dcache(self.dcache.cpu_side)
        self.dcache.mem_side = self.membus.cpu_side_ports

        self.mmucache = MMUCache(size="8KiB")
        self.mmucache.mem_side = self.membus.cpu_side_ports
        self.mmubus = L2XBar(width=64)
        self.mmubus.mem_side_ports = self.mmucache.cpu_side
        cpu.connect_walker_ports(
            self.mmubus.cpu_side_ports, self.mmubus.cpu_side_ports
        )

        if board.has_coherent_io():
            self._setup_io_cache(board)

        if board.get_processor().get_isa() == ISA.X86:
            cpu.connect_interrupt(
                self.membus.mem_side_ports, self.membus.cpu_side_ports
            )
        else:
            cpu.connect_interrupt()


def get_args():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--workload",
        type=str,
        default="hello",
        help="The workload to simulate.",
    )

    parser.add_argument(
        "--prefetcher",
        type=str,
        help="The prefetcher to be used.",
        choices=["None", "NL", "FDIP"]
    )

    parser.add_argument(
        "--arguments",
        type=str,
        default="",
    )

    return parser.parse_args()


def set_workload(board: SimpleBoard):
    if os.path.exists(args.workload):
        # local workload
        board.set_se_binary_workload(
            binary=BinaryResource(args.workload),
            arguments=args.arguments.split(" ")
        )
    else:
        # workload from remote gem5 resource
        board.set_se_binary_workload(
            obtain_resource(workloads[args.workload][args.isa])
        )


def get_simulator(args):

    # only test on X86
    requires(isa_required=ISA.X86)

    # Configure the O3 cpu and X86 ISA and single core
    processor = SimpleProcessor(
        cpu_type=CPUTypes.O3, isa=ISA.X86, num_cores=1
    )
    cpu = processor.cores[0].core

    # FTQ expanded the fetch with from another perspective, thus turn small width of each fetch.
    cpu.fetchBufferSize = 16
    cpu.fetchTargetWidth = 32

    # Set branch predictor
    cpu.branchPred = LTAGE()

    # Create the icache and the prefetcher
    icache = L1ICache(size="32kB")

    if args.prefetcher == "None":
        pass
    elif args.prefetcher == "NL":
        icache.prefetcher = TaggedPrefetcher(degree=1)
    elif args.prefetcher == "FDIP":
        ## Setup the FDP prefetcher
        icache.prefetcher = FetchDirectedInstructionPrefetcher(
            # use_virtual_addresses=True,
            # The FDIP needs to know to which CPU to listent to.
            cpu=cpu,
        )

    # We prefetch instruction directly from memory without L2Cache.
    # MMU is necessary for address translation
    icache.prefetcher.registerMMU(processor.cores[0].core.mmu)

    cache_hierarchy = CacheHierarchy(icache, L1DCache(size="32kB"))

    memory = SingleChannelDDR3_1600(size="32MB")

    # Simple board to run simple SE-mode simulations.
    board = SimpleBoard(
        clk_freq="3GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
    )

    set_workload(board)

    return Simulator(board=board)



if __name__ == "__main__":

    args = get_args()
    simulator = get_simulator(args)

    print(
        "Running workload [{}] with prefetcher [{}]".format(
            args.workload, args.prefetcher
        )
    )

    simulator.run()

    print(
        "Exiting @ tick {} because {}.".format(
            simulator.get_current_tick(), simulator.get_last_exit_event_cause()
        )
    )
