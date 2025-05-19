/*
 * Copyright (c) 2025 All-Hands
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * Fetch Directed Prefetcher declaration.
 */

#ifndef __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__
#define __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__

#include <deque>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "debug/FetchDirectedPrefetch.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

/**
 * Fetch Directed Prefetcher implementation
 * 
 * This prefetcher implements the architecture shown in the figure:
 * - PIQ (Prefetch Instruction Queue): Stores instructions from the fetch stage
 * - FTQ (Fetch Target Queue): Stores branch prediction targets
 * - Prefetch Enqueue: Filters and enqueues prefetch candidates
 * - L2 Cache Prefetch: Initiates prefetches to L2 cache
 * - Prefetch Buffer: Stores prefetched instructions
 */
class FetchDirected : public Queued
{
  private:
    /** Maximum size of the PIQ */
    const unsigned piqSize;

    /** Maximum size of the FTQ */
    const unsigned ftqSize;

    /** Maximum size of the prefetch buffer */
    const unsigned prefetchBufferSize;

    /** Number of prefetch requests to issue per cycle */
    const unsigned prefetchDegree;

    /** Prefetch distance in blocks */
    const unsigned prefetchDistance;
    
    /** Enable/disable decoupled branch predictor */
    const bool enableDecoupledBP;
    
    /** Enable/disable cache eviction tracking */
    const bool enableCacheEvictionTracking;
    
    /** Enable/disable idle port filtering */
    const bool enableIdlePortFiltering;

    /** Prefetch Instruction Queue (PIQ) */
    std::deque<Addr> piq;

    /** Fetch Target Queue (FTQ) */
    std::deque<Addr> ftq;

    /** Prefetch Buffer - maps address to prefetched data */
    std::unordered_map<Addr, bool> prefetchBuffer;

    /** Prefetch filter to avoid redundant prefetches */
    std::unordered_map<Addr, bool> prefetchFilter;

    struct FetchDirectedStats : public statistics::Group
    {
        FetchDirectedStats(statistics::Group *parent);

        /** Number of prefetch candidates identified */
        statistics::Scalar pfCandidatesIdentified;

        /** Number of prefetch candidates filtered */
        statistics::Scalar pfCandidatesFiltered;

        /** Number of prefetches issued to L2 */
        statistics::Scalar pfIssuedToL2;

        /** Number of hits in the prefetch buffer */
        statistics::Scalar pfBufferHits;

        /** PIQ occupancy distribution */
        statistics::Distribution piqOccupancy;

        /** FTQ occupancy distribution */
        statistics::Distribution ftqOccupancy;

        /** Prefetch buffer occupancy distribution */
        statistics::Distribution prefetchBufferOccupancy;
    } statsFetchDirected;

  public:
    FetchDirected(const FetchDirectedPrefetcherParams &p);
    ~FetchDirected() = default;

    /**
     * Calculate the prefetch candidates based on the PIQ and FTQ
     * @param pfi Information about the access that triggered the prefetch
     * @param addresses Vector to be populated with prefetch candidates
     */
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /**
     * Add an address to the PIQ
     * @param addr Address to add to the PIQ
     */
    void addToPIQ(Addr addr);

    /**
     * Add an address to the FTQ
     * @param addr Address to add to the FTQ
     */
    void addToFTQ(Addr addr);
    
    /**
     * Notify the prefetcher of a cache eviction
     * @param addr Address that was evicted from the cache
     */
    void notifyCacheEviction(Addr addr);

    /**
     * Check if an address is in the prefetch buffer
     * @param addr Address to check
     * @return True if the address is in the prefetch buffer
     */
    bool isInPrefetchBuffer(Addr addr);

    /**
     * Add an address to the prefetch buffer
     * @param addr Address to add to the prefetch buffer
     */
    void addToPrefetchBuffer(Addr addr);

    /**
     * Remove an address from the prefetch buffer
     * @param addr Address to remove from the prefetch buffer
     */
    void removeFromPrefetchBuffer(Addr addr);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__