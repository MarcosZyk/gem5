# Fetch Directed Prefetcher

## Overview

The Fetch Directed Prefetcher is an instruction prefetcher that coordinates between the CPU fetch stage and the cache prefetcher to improve instruction prefetching. It implements the architecture shown in Figure 1 of the design document.

## Architecture Components

1. **PIQ (Prefetch Instruction Queue)**: Stores prefetch candidates from the CPU fetch stage.
2. **FTQ (Fetch Target Queue)**: Stores fetch targets from the branch predictor.
3. **Prefetch Enqueue**: Filters prefetch candidates to avoid redundant prefetches.
4. **L2 Cache Prefetch**: Issues prefetch requests to the L2 cache.
5. **Prefetch Buffer**: Stores prefetched instructions for quick access.

## Implementation Details

The prefetcher is implemented as a class that inherits from the `Queued` prefetcher base class in gem5. It maintains three main data structures:

1. **PIQ**: A deque that stores addresses from the CPU fetch stage.
2. **FTQ**: A deque that stores branch prediction targets.
3. **Prefetch Buffer**: A map that stores prefetched addresses.

The prefetcher also includes a filtering mechanism to avoid redundant prefetches.

## Configuration Parameters

The prefetcher can be configured with the following parameters:

- `piq_size`: Maximum size of the PIQ (default: 16)
- `ftq_size`: Maximum size of the FTQ (default: 16)
- `prefetch_buffer_size`: Maximum size of the prefetch buffer (default: 32)
- `prefetch_degree`: Number of prefetch requests to issue per cycle (default: 4)
- `prefetch_distance`: Prefetch distance in blocks (default: 2)

## Usage

To use the Fetch Directed Prefetcher in a gem5 simulation, configure the L2 cache to use it:

```python
system.l2.prefetcher = FetchDirectedPrefetcher(
    piq_size=16,
    ftq_size=16,
    prefetch_buffer_size=32,
    prefetch_degree=4,
    prefetch_distance=2
)
```

A sample configuration script is provided in `configs/example/fetch_directed_prefetcher.py`.

## Statistics

The prefetcher collects the following statistics:

- `pfCandidatesIdentified`: Number of prefetch candidates identified
- `pfCandidatesFiltered`: Number of prefetch candidates filtered
- `pfIssuedToL2`: Number of prefetches issued to L2
- `pfBufferHits`: Number of hits in the prefetch buffer
- `piqOccupancy`: PIQ occupancy distribution
- `ftqOccupancy`: FTQ occupancy distribution
- `prefetchBufferOccupancy`: Prefetch buffer occupancy distribution

## Debugging

To enable debug output for the Fetch Directed Prefetcher, use the `FetchDirectedPrefetch` debug flag:

```
./build/X86/gem5.opt --debug-flags=FetchDirectedPrefetch configs/example/fetch_directed_prefetcher.py
```

## Future Work

1. Improve the prefetch filtering mechanism to be more selective.
2. Add support for more sophisticated branch prediction integration.
3. Implement adaptive prefetch distance based on program behavior.
4. Add support for multi-core systems with shared caches.