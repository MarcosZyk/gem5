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

#include "mem/cache/prefetch/fdip.hh"

#include <algorithm>
#include <cassert>
#include <functional>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/pred/tage.hh"
#include "debug/FDIP.hh"
#include "mem/cache/base.hh"
#include "params/FDIPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

FDIP::FDIP(const FDIPrefetcherParams &p)
    : Queued(p),
      degree(p.degree),
      useTAGE(p.use_tage),
      branchPred(nullptr),
      ownBranchPred(false),
      threadId(p.thread_id),
      lineSize(p.line_size),
      maxLookAhead(p.max_lookahead),
      cleanupInterval(p.cleanup_interval),
      lastCleanupTick(0),
      confidenceThreshold(p.confidence_threshold),
      piqSize(p.piq_size),
      ftqSize(p.ftq_size),
      prefetchBufferSize(p.prefetch_buffer_size)
{
    // Ensure parameters are valid
    assert(degree > 0);
    assert(lineSize > 0);
    assert(maxLookAhead > 0);
    assert(confidenceThreshold <= 100);
    assert(piqSize > 0);
    assert(ftqSize > 0);
    assert(prefetchBufferSize > 0);
    
    // Create a dedicated branch predictor if requested
    if (p.create_dedicated_predictor) {
        if (useTAGE) {
            // Create a TAGE branch predictor
            auto tageBP = new branch_prediction::TAGE(p.tage_params);
            branchPred = tageBP;
        } else {
            // Create a tournament branch predictor (default)
            auto tournamentBP = new branch_prediction::BPredUnit(p.tournament_params);
            branchPred = tournamentBP;
        }
        ownBranchPred = true;
        DPRINTF(FDIP, "Created dedicated branch predictor\n");
        
        // Register callbacks for branch prediction events
        if (branchPred) {
            using namespace std::placeholders;
            branchPred->setMispredictionHandler(
                std::bind(&FDIP::notifyBranchMisprediction, this, _1, _2, _3));
            branchPred->setCorrectPredictionHandler(
                std::bind(&FDIP::notifyCorrectPrediction, this, _1, _2, _3));
            DPRINTF(FDIP, "Registered branch prediction callbacks\n");
        }
    }
}

FDIP::~FDIP()
{
    // Clean up the branch predictor if we own it
    if (ownBranchPred && branchPred) {
        delete branchPred;
        branchPred = nullptr;
    }
}

void
FDIP::setBranchPredictor(branch_prediction::BPredUnit *bp)
{
    // Only set the branch predictor if we don't already have one
    if (!branchPred) {
        branchPred = bp;
        ownBranchPred = false;
        DPRINTF(FDIP, "Using external branch predictor\n");
        
        // Register callbacks for branch prediction events
        if (branchPred) {
            using namespace std::placeholders;
            branchPred->setMispredictionHandler(
                std::bind(&FDIP::notifyBranchMisprediction, this, _1, _2, _3));
            branchPred->setCorrectPredictionHandler(
                std::bind(&FDIP::notifyCorrectPrediction, this, _1, _2, _3));
            DPRINTF(FDIP, "Registered branch prediction callbacks\n");
        }
    } else {
        DPRINTF(FDIP, "Branch predictor already set, ignoring\n");
    }
}

void
FDIP::addToPIQ(Addr pc, Addr targetAddr, unsigned confidence)
{
    // Create a new PIQ entry
    PIQEntry entry(pc, targetAddr, confidence, curTick());
    
    // Add to the PIQ, maintaining size limit
    piq.push_back(entry);
    if (piq.size() > piqSize) {
        piq.pop_front();
    }
    
    DPRINTF(FDIP, "Added to PIQ: PC 0x%x, target 0x%x, confidence %u\n",
            pc, targetAddr, confidence);
}

void
FDIP::addToFTQ(Addr pc, Addr targetAddr, bool taken, unsigned confidence)
{
    // Create a new FTQ entry
    FTQEntry entry(pc, targetAddr, taken, confidence, curTick());
    
    // Add to the FTQ, maintaining size limit
    ftq.push_back(entry);
    if (ftq.size() > ftqSize) {
        ftq.pop_front();
    }
    
    DPRINTF(FDIP, "Added to FTQ: PC 0x%x, target 0x%x, taken %d, confidence %u\n",
            pc, targetAddr, taken, confidence);
}

void
FDIP::addToPrefetchBuffer(Addr addr, unsigned priority)
{
    // Create a new prefetch buffer entry
    PrefetchBufferEntry entry(addr, curTick(), priority);
    
    // Add to the prefetch buffer
    prefetchBuffer.push_back(entry);
    
    // Sort by priority (lower value = higher priority)
    std::sort(prefetchBuffer.begin(), prefetchBuffer.end(),
              [](const PrefetchBufferEntry &a, const PrefetchBufferEntry &b) {
                  return a.priority < b.priority;
              });
    
    // Maintain size limit
    if (prefetchBuffer.size() > prefetchBufferSize) {
        // Remove the lowest priority entry (which will be at the end after sorting)
        prefetchBuffer.pop_back();
    }
    
    DPRINTF(FDIP, "Added to Prefetch Buffer: addr 0x%x, priority %u\n",
            addr, priority);
}

std::vector<Addr>
FDIP::predictBranchTargets(Addr pc, unsigned numBranches)
{
    std::vector<Addr> targets;
    
    if (!branchPred) {
        DPRINTF(FDIP, "Branch predictor not set, cannot predict targets\n");
        return targets;
    }
    
    // No valid stream, predict new targets
    Addr currentPC = pc;
    
    // Create a temporary PCState for branch prediction
    std::unique_ptr<PCStateBase> pcState(branchPred->getInstPC(currentPC));
    
    // Predict up to numBranches branches
    for (unsigned i = 0; i < numBranches; i++) {
        void *bp_history = nullptr;
        
        // Check confidence for this prediction
        unsigned confidence = getPredictionConfidence(currentPC);
        if (confidence < confidenceThreshold) {
            DPRINTF(FDIP, "Prediction confidence %d below threshold %d for PC 0x%x\n",
                    confidence, confidenceThreshold, currentPC);
            break;
        }
        
        // Use the branch predictor to predict the next branch
        bool taken = branchPred->lookup(threadId, currentPC, bp_history);
        
        if (taken) {
            // Get the predicted target
            Addr target = branchPred->getTargetAddr(currentPC, bp_history);
            targets.push_back(target);
            
            // Add to FTQ
            addToFTQ(currentPC, target, true, confidence);
            
            // Update the current PC to the predicted target
            currentPC = target;
        } else {
            // If not taken, assume sequential execution (next instruction)
            // For simplicity, we'll just increment by 4 (typical instruction size)
            Addr nextPC = currentPC + 4;
            
            // Add to FTQ
            addToFTQ(currentPC, nextPC, false, confidence);
            
            currentPC = nextPC;
        }
        
        // Clean up branch predictor history
        if (bp_history) {
            branchPred->squash(threadId, bp_history);
        }
    }
    
    return targets;
}

bool
FDIP::isRecentlyPrefetched(Addr addr)
{
    return prefetchedAddresses.find(addr) != prefetchedAddresses.end();
}

void
FDIP::markPrefetched(Addr addr)
{
    prefetchedAddresses[addr] = curTick();
}

void
FDIP::cleanupPrefetchedAddresses(Tick currentTick)
{
    // Only clean up periodically to reduce overhead
    if (currentTick - lastCleanupTick < cleanupInterval) {
        return;
    }
    
    lastCleanupTick = currentTick;
    
    // Remove entries older than cleanupInterval
    auto it = prefetchedAddresses.begin();
    while (it != prefetchedAddresses.end()) {
        if (currentTick - it->second > cleanupInterval) {
            it = prefetchedAddresses.erase(it);
        } else {
            ++it;
        }
    }
    
    // Also clean up old PIQ entries
    while (!piq.empty() && currentTick - piq.front().timestamp > cleanupInterval) {
        piq.pop_front();
    }
    
    // Clean up old FTQ entries
    while (!ftq.empty() && currentTick - ftq.front().timestamp > cleanupInterval) {
        ftq.pop_front();
    }
    
    // Clean up old prefetch buffer entries
    auto bufferIt = prefetchBuffer.begin();
    while (bufferIt != prefetchBuffer.end()) {
        if (currentTick - bufferIt->timestamp > cleanupInterval) {
            bufferIt = prefetchBuffer.erase(bufferIt);
        } else {
            ++bufferIt;
        }
    }
}

unsigned
FDIP::getPredictionConfidence(Addr pc)
{
    // If we have no history for this PC, return a default confidence
    auto it = predictionAccuracy.find(pc);
    if (it == predictionAccuracy.end()) {
        return 50; // Default 50% confidence
    }
    
    // Calculate confidence based on prediction history
    unsigned correct = it->second.first;
    unsigned total = it->second.second;
    
    // Avoid division by zero
    if (total == 0) {
        return 50;
    }
    
    // Return confidence as a percentage
    return (correct * 100) / total;
}

std::vector<Addr>
FDIP::applyFiltration(const std::vector<Addr> &candidates, const CacheAccessor &cache)
{
    std::vector<Addr> filtered;
    
    for (Addr addr : candidates) {
        // Convert to cache line address
        Addr lineAddr = blockAddress(addr);
        
        // Apply filtration mechanisms:
        // 1. Skip if already in cache
        // 2. Skip if already in miss queue
        // 3. Skip if recently prefetched
        // 4. Skip if confidence is too low
        
        // Check if this line is already in the cache or recently prefetched
        if (cache.inCache(lineAddr, false) || 
            cache.inMissQueue(lineAddr, false) ||
            isRecentlyPrefetched(lineAddr)) {
            continue;
        }
        
        // Add to filtered list
        filtered.push_back(lineAddr);
    }
    
    return filtered;
}

void
FDIP::processPIQ(const CacheAccessor &cache, std::vector<AddrPriority> &addresses)
{
    DPRINTF(FDIP, "Processing PIQ with %d entries\n", piq.size());
    
    // Process PIQ entries to generate L2 cache prefetches
    std::vector<Addr> candidates;
    
    for (const auto &entry : piq) {
        // Skip low confidence entries
        if (entry.confidence < confidenceThreshold) {
            continue;
        }
        
        // Add target address to candidates
        candidates.push_back(entry.targetAddr);
        
        // Also add a few sequential lines after the target
        for (unsigned i = 1; i <= degree; i++) {
            candidates.push_back(entry.targetAddr + i * lineSize);
        }
    }
    
    // Apply filtration mechanisms
    std::vector<Addr> filtered = applyFiltration(candidates, cache);
    
    // Add filtered candidates to prefetch addresses
    unsigned priority = 0;
    for (Addr addr : filtered) {
        // Add to prefetch addresses with priority
        addresses.push_back(AddrPriority(addr, priority));
        
        // Mark as prefetched
        markPrefetched(addr);
        
        // Add to prefetch buffer
        addToPrefetchBuffer(addr, priority);
        
        DPRINTF(FDIP, "L2 Cache Prefetch from PIQ: 0x%x, priority %u\n", 
                addr, priority);
        
        priority++;
        
        // Limit the number of prefetches
        if (priority >= degree) {
            break;
        }
    }
}

void
FDIP::processFTQ(const CacheAccessor &cache, std::vector<AddrPriority> &addresses)
{
    DPRINTF(FDIP, "Processing FTQ with %d entries\n", ftq.size());
    
    // Process FTQ entries to generate prefetch candidates
    std::vector<Addr> candidates;
    
    for (const auto &entry : ftq) {
        // Skip low confidence entries
        if (entry.confidence < confidenceThreshold) {
            continue;
        }
        
        // Add target address to candidates
        candidates.push_back(entry.targetAddr);
        
        // For taken branches, also add a few sequential lines after the target
        if (entry.taken) {
            for (unsigned i = 1; i <= degree; i++) {
                candidates.push_back(entry.targetAddr + i * lineSize);
            }
        }
        
        // Add to PIQ for L2 cache prefetching
        addToPIQ(entry.pc, entry.targetAddr, entry.confidence);
    }
    
    // Apply filtration mechanisms
    std::vector<Addr> filtered = applyFiltration(candidates, cache);
    
    // Add filtered candidates to prefetch addresses
    unsigned priority = 0;
    for (Addr addr : filtered) {
        // Add to prefetch addresses with priority
        addresses.push_back(AddrPriority(addr, priority));
        
        // Mark as prefetched
        markPrefetched(addr);
        
        DPRINTF(FDIP, "Prefetch from FTQ: 0x%x, priority %u\n", 
                addr, priority);
        
        priority++;
        
        // Limit the number of prefetches
        if (priority >= degree) {
            break;
        }
    }
}

void
FDIP::processPrefetchBuffer(const CacheAccessor &cache, std::vector<AddrPriority> &addresses)
{
    DPRINTF(FDIP, "Processing Prefetch Buffer with %d entries\n", prefetchBuffer.size());
    
    // Process prefetch buffer entries to feed instruction fetch
    unsigned count = 0;
    
    for (const auto &entry : prefetchBuffer) {
        // Skip if already in cache or miss queue
        if (cache.inCache(entry.addr, false) || 
            cache.inMissQueue(entry.addr, false)) {
            continue;
        }
        
        // Add to prefetch addresses with original priority
        addresses.push_back(AddrPriority(entry.addr, entry.priority));
        
        DPRINTF(FDIP, "Prefetch from Buffer: 0x%x, priority %u\n", 
                entry.addr, entry.priority);
        
        count++;
        
        // Limit the number of prefetches
        if (count >= degree) {
            break;
        }
    }
}

void
FDIP::notifyBranchMisprediction(Addr pc, Addr actualTarget, unsigned confidence)
{
    DPRINTF(FDIP, "Branch misprediction at PC 0x%x, actual target 0x%x, confidence %u\n",
            pc, actualTarget, confidence);
    
    // Update prediction accuracy
    auto it = predictionAccuracy.find(pc);
    if (it == predictionAccuracy.end()) {
        // First prediction for this PC
        predictionAccuracy[pc] = std::make_pair(0, 1);
    } else {
        // Increment total predictions
        it->second.second++;
    }
    
    // Add to FTQ with the actual target
    addToFTQ(pc, actualTarget, true, confidence);
    
    // Add to PIQ for L2 cache prefetching
    addToPIQ(pc, actualTarget, confidence);
}

void
FDIP::notifyCorrectPrediction(Addr pc, Addr target, unsigned confidence)
{
    DPRINTF(FDIP, "Correct branch prediction at PC 0x%x, target 0x%x, confidence %u\n",
            pc, target, confidence);
    
    // Update prediction accuracy
    auto it = predictionAccuracy.find(pc);
    if (it == predictionAccuracy.end()) {
        // First prediction for this PC
        predictionAccuracy[pc] = std::make_pair(1, 1);
    } else {
        // Increment correct and total predictions
        it->second.first++;
        it->second.second++;
    }
    
    // Add to FTQ
    addToFTQ(pc, target, true, confidence);
    
    // Add to PIQ for L2 cache prefetching
    addToPIQ(pc, target, confidence);
}

void
FDIP::calculatePrefetch(const PrefetchInfo &pfi,
                       std::vector<AddrPriority> &addresses,
                       const CacheAccessor &cache)
{
    // Clean up old prefetched addresses and queue entries
    cleanupPrefetchedAddresses(curTick());
    
    // If we don't have a branch predictor, fall back to next-line prefetching
    if (!branchPred) {
        DPRINTF(FDIP, "No branch predictor available, falling back to next-line prefetching\n");
        
        Addr current_addr = blockAddress(pfi.getAddr());
        
        // Prefetch the next 'degree' cache lines
        for (unsigned d = 1; d <= degree; d++) {
            Addr pf_addr = current_addr + d * lineSize;
            
            // Check if this line is already in the cache or recently prefetched
            if (cache.inCache(pf_addr, pfi.isSecure()) || 
                cache.inMissQueue(pf_addr, pfi.isSecure()) ||
                isRecentlyPrefetched(pf_addr)) {
                continue;
            }
            
            addresses.push_back(AddrPriority(pf_addr, 0));
            markPrefetched(pf_addr);
            
            DPRINTF(FDIP, "Next-line prefetch: 0x%x\n", pf_addr);
        }
        
        return;
    }
    
    // Get the current PC from the prefetch info
    Addr pc = pfi.getPC();
    if (!pfi.hasPC()) {
        DPRINTF(FDIP, "No PC available for FDIP, falling back to next-line prefetching\n");
        
        // Fall back to next-line prefetching if no PC is available
        Addr current_addr = blockAddress(pfi.getAddr());
        
        for (unsigned d = 1; d <= degree; d++) {
            Addr pf_addr = current_addr + d * lineSize;
            
            if (cache.inCache(pf_addr, pfi.isSecure()) || 
                cache.inMissQueue(pf_addr, pfi.isSecure()) ||
                isRecentlyPrefetched(pf_addr)) {
                continue;
            }
            
            addresses.push_back(AddrPriority(pf_addr, 0));
            markPrefetched(pf_addr);
            
            DPRINTF(FDIP, "Next-line prefetch (no PC): 0x%x\n", pf_addr);
        }
        
        return;
    }
    
    // Predict branch targets and populate FTQ
    std::vector<Addr> targets = predictBranchTargets(pc, maxLookAhead);
    
    DPRINTF(FDIP, "FDIP for PC 0x%x predicted %d targets\n", pc, targets.size());
    
    // If no targets were predicted, fall back to next-line prefetching
    if (targets.empty()) {
        Addr current_addr = blockAddress(pfi.getAddr());
        
        for (unsigned d = 1; d <= degree; d++) {
            Addr pf_addr = current_addr + d * lineSize;
            
            if (cache.inCache(pf_addr, pfi.isSecure()) || 
                cache.inMissQueue(pf_addr, pfi.isSecure()) ||
                isRecentlyPrefetched(pf_addr)) {
                continue;
            }
            
            addresses.push_back(AddrPriority(pf_addr, 0));
            markPrefetched(pf_addr);
            
            DPRINTF(FDIP, "Next-line prefetch (no targets): 0x%x\n", pf_addr);
        }
        
        return;
    }
    
    // Process the PIQ to generate L2 cache prefetches
    processPIQ(cache, addresses);
    
    // Process the FTQ to generate prefetch candidates
    processFTQ(cache, addresses);
    
    // Process the prefetch buffer to feed instruction fetch
    processPrefetchBuffer(cache, addresses);
}

} // namespace prefetch
} // namespace gem5

} // namespace prefetch
} // namespace gem5