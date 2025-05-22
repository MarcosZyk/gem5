/*
 * Copyright (c) 2024 All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 */

/**
 * @file
 * Fetch Directed Instruction Prefetching (FDIP) implementation.
 * This prefetcher uses branch predictor results to guide instruction prefetching.
 */

#ifndef __MEM_CACHE_PREFETCH_FDIP_HH__
#define __MEM_CACHE_PREFETCH_FDIP_HH__

#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "cpu/pred/bpred_unit.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/FDIPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

class FDIP : public Queued
{
  private:
    /** The number of prefetch degrees (how many blocks to prefetch) */
    const unsigned degree;

    /** Whether to use the TAGE branch predictor for prefetching */
    const bool useTAGE;

    /** Pointer to the branch predictor unit */
    branch_prediction::BPredUnit *branchPred;

    /** Thread ID to use for branch prediction (single thread support) */
    const ThreadID threadId;

    /** Cache line size in bytes */
    const unsigned lineSize;

    /** Maximum number of instructions to look ahead for prefetching */
    const unsigned maxLookAhead;

    /** 
     * Map to track prefetched addresses to avoid redundant prefetches
     * Key: prefetched address, Value: timestamp when it was prefetched
     */
    std::unordered_map<Addr, Tick> prefetchedAddresses;

    /** 
     * Cleanup interval for the prefetchedAddresses map
     * (remove entries older than this many ticks)
     */
    const Tick cleanupInterval;

    /** Last time the prefetchedAddresses map was cleaned up */
    Tick lastCleanupTick;

    /**
     * Predict the next N branch targets using the branch predictor
     * @param pc Current program counter
     * @param numBranches Number of branches to predict ahead
     * @return Vector of predicted target addresses
     */
    std::vector<Addr> predictBranchTargets(Addr pc, unsigned numBranches);

    /**
     * Check if an address has been recently prefetched
     * @param addr Address to check
     * @return True if the address has been recently prefetched
     */
    bool isRecentlyPrefetched(Addr addr);

    /**
     * Mark an address as prefetched
     * @param addr Address to mark
     */
    void markPrefetched(Addr addr);

    /**
     * Clean up old entries from the prefetchedAddresses map
     * @param currentTick Current simulation tick
     */
    void cleanupPrefetchedAddresses(Tick currentTick);

  public:
    FDIP(const FDIPrefetcherParams &p);

    /**
     * Set the branch predictor to use for prefetching
     * @param bp Pointer to the branch predictor unit
     */
    void setBranchPredictor(branch_prediction::BPredUnit *bp);

    /**
     * Calculate prefetches based on the provided access.
     * @param pfi Access information
     * @param addresses Vector to store the generated prefetch addresses
     * @param cache Cache accessor to check for prefetch hits
     */
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FDIP_HH__