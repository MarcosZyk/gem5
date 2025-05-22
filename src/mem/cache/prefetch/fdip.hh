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
#include <memory>
#include <queue>
#include <deque>

#include "base/types.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/tage.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/FDIPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

/**
 * Fetch Directed Instruction Prefetching (FDIP) implementation.
 * 
 * This prefetcher uses branch predictor results to guide instruction prefetching.
 * It implements the FDIP architecture with PIQ, FTQ, Prefetch Enqueue, 
 * L2 Cache Prefetch, and Prefetch Buffer components.
 */
class FDIP : public Queued
{
  private:
    /** The number of prefetch degrees (how many blocks to prefetch) */
    const unsigned degree;

    /** Whether to use the TAGE branch predictor for prefetching */
    const bool useTAGE;

    /** Pointer to the branch predictor unit */
    branch_prediction::BPredUnit *branchPred;

    /** Whether we own the branch predictor (and need to delete it) */
    bool ownBranchPred;

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
     * Track branch prediction accuracy to adjust prefetching strategy
     * Key: PC, Value: pair of (correct predictions, total predictions)
     */
    std::unordered_map<Addr, std::pair<unsigned, unsigned>> predictionAccuracy;

    /** Minimum confidence threshold to issue prefetches (0-100) */
    const unsigned confidenceThreshold;

    /** Maximum size of the PIQ (Program Information Queue) */
    const unsigned piqSize;

    /** Maximum size of the FTQ (Fetch Target Queue) */
    const unsigned ftqSize;

    /** Maximum size of the Prefetch Buffer */
    const unsigned prefetchBufferSize;
    
    /** Last PC used for ahead-of-time branch prediction */
    Addr lastPredictionPC;
    
    /** How far ahead the branch predictor should work compared to the prefetcher */
    const unsigned branchPredictorLookahead;

    /** 
     * Program Information Queue (PIQ)
     * Stores information about program execution for prefetching
     */
    struct PIQEntry {
        Addr pc;           // Program counter
        Addr targetAddr;   // Target address
        unsigned confidence; // Prediction confidence
        Tick timestamp;    // When this entry was added
        
        PIQEntry(Addr _pc, Addr _target, unsigned _conf, Tick _time)
            : pc(_pc), targetAddr(_target), confidence(_conf), timestamp(_time) {}
    };
    
    /** PIQ implementation as a circular buffer */
    std::deque<PIQEntry> piq;

    /**
     * Fetch Target Queue (FTQ)
     * Stores branch prediction targets from the branch predictor
     */
    struct FTQEntry {
        Addr pc;           // Program counter
        Addr targetAddr;   // Target address
        bool taken;        // Whether the branch was predicted taken
        unsigned confidence; // Prediction confidence
        Tick timestamp;    // When this entry was added
        
        FTQEntry(Addr _pc, Addr _target, bool _taken, unsigned _conf, Tick _time)
            : pc(_pc), targetAddr(_target), taken(_taken), confidence(_conf), timestamp(_time) {}
    };
    
    /** FTQ implementation as a circular buffer */
    std::deque<FTQEntry> ftq;

    /**
     * Prefetch Buffer
     * Stores prefetched cache lines before they are consumed by instruction fetch
     */
    struct PrefetchBufferEntry {
        Addr addr;         // Prefetched address
        Tick timestamp;    // When this entry was added
        unsigned priority; // Priority of this prefetch (lower is higher priority)
        
        PrefetchBufferEntry(Addr _addr, Tick _time, unsigned _prio)
            : addr(_addr), timestamp(_time), priority(_prio) {}
    };
    
    /** Prefetch Buffer implementation as a priority queue */
    std::vector<PrefetchBufferEntry> prefetchBuffer;

    /**
     * Add an entry to the PIQ
     * @param pc Program counter
     * @param targetAddr Target address
     * @param confidence Prediction confidence
     */
    void addToPIQ(Addr pc, Addr targetAddr, unsigned confidence);

    /**
     * Add an entry to the FTQ
     * @param pc Program counter
     * @param targetAddr Target address
     * @param taken Whether the branch was predicted taken
     * @param confidence Prediction confidence
     */
    void addToFTQ(Addr pc, Addr targetAddr, bool taken, unsigned confidence);

    /**
     * Add an entry to the Prefetch Buffer
     * @param addr Address to prefetch
     * @param priority Priority of this prefetch (lower is higher priority)
     */
    void addToPrefetchBuffer(Addr addr, unsigned priority);

    /**
     * Process the PIQ to generate prefetches for the L2 cache
     * @param cache Cache accessor to check for prefetch hits
     * @param addresses Vector to store the generated prefetch addresses
     */
    void processPIQ(const CacheAccessor &cache, std::vector<AddrPriority> &addresses);

    /**
     * Process the FTQ to generate prefetch candidates for the Prefetch Enqueue
     * @param cache Cache accessor to check for prefetch hits
     * @param addresses Vector to store the generated prefetch addresses
     */
    void processFTQ(const CacheAccessor &cache, std::vector<AddrPriority> &addresses);

    /**
     * Process the Prefetch Buffer to feed the Instruction Fetch
     * @param cache Cache accessor to check for prefetch hits
     * @param addresses Vector to store the generated prefetch addresses
     */
    void processPrefetchBuffer(const CacheAccessor &cache, std::vector<AddrPriority> &addresses);

    /**
     * Run branch prediction ahead of time to populate the FTQ
     * This method should be called periodically to ensure the branch predictor
     * works ahead of the prefetcher
     * @param pc Current program counter
     * @param numBranches Number of branches to predict ahead
     */
    void runAheadBranchPrediction(Addr pc, unsigned numBranches);
    
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

    /**
     * Get the confidence for a branch prediction at a given PC
     * @param pc Program counter
     * @return Confidence value (0-100)
     */
    unsigned getPredictionConfidence(Addr pc);

    /**
     * Apply filtration mechanisms to prefetch candidates
     * @param candidates Vector of prefetch candidates
     * @param cache Cache accessor to check for prefetch hits
     * @return Vector of filtered prefetch candidates
     */
    std::vector<Addr> applyFiltration(const std::vector<Addr> &candidates, 
                                     const CacheAccessor &cache);

  public:
    FDIP(const FDIPrefetcherParams &p);
    ~FDIP();

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

    /**
     * Notify the prefetcher about a branch misprediction
     * @param pc Program counter of the mispredicted branch
     * @param actualTarget Actual target of the branch
     * @param confidence Confidence of the prediction (0-100)
     */
    void notifyBranchMisprediction(Addr pc, Addr actualTarget, unsigned confidence);

    /**
     * Notify the prefetcher about a correct branch prediction
     * @param pc Program counter of the correctly predicted branch
     * @param target Target of the branch
     * @param confidence Confidence of the prediction (0-100)
     */
    void notifyCorrectPrediction(Addr pc, Addr target, unsigned confidence);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FDIP_HH__