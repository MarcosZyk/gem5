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
 * Fetch Directed Prefetcher implementation.
 */

#include "mem/cache/prefetch/fetch_directed.hh"

#include "debug/FetchDirectedPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FetchDirected::FetchDirectedStats::FetchDirectedStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(pfCandidatesIdentified, statistics::units::Count::get(),
               "Number of prefetch candidates identified"),
      ADD_STAT(pfCandidatesFiltered, statistics::units::Count::get(),
               "Number of prefetch candidates filtered"),
      ADD_STAT(pfIssuedToL2, statistics::units::Count::get(),
               "Number of prefetches issued to L2"),
      ADD_STAT(pfBufferHits, statistics::units::Count::get(),
               "Number of hits in the prefetch buffer"),
      ADD_STAT(piqOccupancy, statistics::units::Count::get(),
               "PIQ occupancy distribution"),
      ADD_STAT(ftqOccupancy, statistics::units::Count::get(),
               "FTQ occupancy distribution"),
      ADD_STAT(prefetchBufferOccupancy, statistics::units::Count::get(),
               "Prefetch buffer occupancy distribution")
{
}

FetchDirected::FetchDirected(const FetchDirectedPrefetcherParams &p)
    : Queued(p),
      piqSize(p.piq_size),
      ftqSize(p.ftq_size),
      prefetchBufferSize(p.prefetch_buffer_size),
      prefetchDegree(p.prefetch_degree),
      prefetchDistance(p.prefetch_distance),
      statsFetchDirected(this)
{
}

void
FetchDirected::calculatePrefetch(const PrefetchInfo &pfi,
                                std::vector<AddrPriority> &addresses,
                                const CacheAccessor &cache)
{
    Addr addr = pfi.getAddr();
    Addr pc = pfi.getPC();
    bool is_secure = pfi.isSecure();

    // Record statistics for queue occupancy
    statsFetchDirected.piqOccupancy.sample(piq.size());
    statsFetchDirected.ftqOccupancy.sample(ftq.size());
    statsFetchDirected.prefetchBufferOccupancy.sample(prefetchBuffer.size());

    // Check if this is an instruction fetch
    if (pfi.isCacheMiss() && pfi.req->isInstFetch()) {
        // Add the current address to the PIQ
        addToPIQ(addr);

        // Process the PIQ and FTQ to generate prefetch candidates
        if (!piq.empty()) {
            // Get the current FTQ prefetch candidate if available
            Addr prefetch_target = 0;
            if (!ftq.empty()) {
                prefetch_target = ftq.front();
            }

            // Generate prefetch candidates
            for (unsigned i = 0; i < prefetchDegree; i++) {
                Addr pf_addr = 0;

                // If we have a branch target, use it as a prefetch candidate
                if (prefetch_target != 0) {
                    pf_addr = prefetch_target;
                    // Move to the next block for subsequent prefetches
                    prefetch_target += blkSize;
                } else {
                    // Otherwise, prefetch sequential blocks ahead of the current address
                    pf_addr = addr + (i + prefetchDistance) * blkSize;
                }

                // Check if the address is already in the prefetch filter
                if (prefetchFilter.find(pf_addr) != prefetchFilter.end()) {
                    DPRINTF(FetchDirectedPrefetch, 
                            "Prefetch candidate %#x filtered out\n", pf_addr);
                    statsFetchDirected.pfCandidatesFiltered++;
                    continue;
                }

                // Check if the address is already in the cache
                if (cache.inCache(pf_addr, is_secure)) {
                    DPRINTF(FetchDirectedPrefetch, 
                            "Prefetch candidate %#x already in cache\n", pf_addr);
                    statsFetchDirected.pfCandidatesFiltered++;
                    continue;
                }

                // Check if the address is already in an MSHR
                if (cache.inMissQueue(pf_addr, is_secure)) {
                    DPRINTF(FetchDirectedPrefetch, 
                            "Prefetch candidate %#x already in MSHR\n", pf_addr);
                    statsFetchDirected.pfCandidatesFiltered++;
                    continue;
                }

                // Add the prefetch candidate to the list
                DPRINTF(FetchDirectedPrefetch, 
                        "Adding prefetch candidate %#x to list\n", pf_addr);
                addresses.push_back(AddrPriority(pf_addr, 0));
                prefetchFilter[pf_addr] = true;
                statsFetchDirected.pfCandidatesIdentified++;
                statsFetchDirected.pfIssuedToL2++;

                // Add to prefetch buffer
                addToPrefetchBuffer(pf_addr);
            }
        }
    }

    // Check if this is a hit in the prefetch buffer
    if (isInPrefetchBuffer(addr)) {
        DPRINTF(FetchDirectedPrefetch, "Hit in prefetch buffer for %#x\n", addr);
        statsFetchDirected.pfBufferHits++;
        removeFromPrefetchBuffer(addr);
    }
}

void
FetchDirected::addToPIQ(Addr addr)
{
    // Add address to PIQ if not already present
    if (std::find(piq.begin(), piq.end(), addr) == piq.end()) {
        piq.push_back(addr);
        DPRINTF(FetchDirectedPrefetch, "Added %#x to PIQ\n", addr);

        // Maintain PIQ size
        while (piq.size() > piqSize) {
            DPRINTF(FetchDirectedPrefetch, "Removing %#x from PIQ\n", piq.front());
            piq.pop_front();
        }
    }
}

void
FetchDirected::addToFTQ(Addr addr)
{
    // Add address to FTQ if not already present
    if (std::find(ftq.begin(), ftq.end(), addr) == ftq.end()) {
        ftq.push_back(addr);
        DPRINTF(FetchDirectedPrefetch, "Added %#x to FTQ\n", addr);

        // Maintain FTQ size
        while (ftq.size() > ftqSize) {
            DPRINTF(FetchDirectedPrefetch, "Removing %#x from FTQ\n", ftq.front());
            ftq.pop_front();
        }
    }
}

bool
FetchDirected::isInPrefetchBuffer(Addr addr)
{
    return prefetchBuffer.find(addr) != prefetchBuffer.end();
}

void
FetchDirected::addToPrefetchBuffer(Addr addr)
{
    // Add address to prefetch buffer
    prefetchBuffer[addr] = true;
    DPRINTF(FetchDirectedPrefetch, "Added %#x to prefetch buffer\n", addr);

    // Maintain prefetch buffer size
    if (prefetchBuffer.size() > prefetchBufferSize) {
        // Remove the oldest entry (this is a simple approach; could be more sophisticated)
        auto it = prefetchBuffer.begin();
        DPRINTF(FetchDirectedPrefetch, "Removing %#x from prefetch buffer\n", it->first);
        prefetchBuffer.erase(it);
    }
}

void
FetchDirected::removeFromPrefetchBuffer(Addr addr)
{
    // Remove address from prefetch buffer
    if (prefetchBuffer.find(addr) != prefetchBuffer.end()) {
        DPRINTF(FetchDirectedPrefetch, "Removed %#x from prefetch buffer\n", addr);
        prefetchBuffer.erase(addr);
    }
}

} // namespace prefetch
} // namespace gem5