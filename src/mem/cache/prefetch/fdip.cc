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

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
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
      threadId(p.thread_id),
      lineSize(p.line_size),
      maxLookAhead(p.max_lookahead),
      cleanupInterval(p.cleanup_interval),
      lastCleanupTick(0)
{
    // Ensure parameters are valid
    assert(degree > 0);
    assert(lineSize > 0);
    assert(maxLookAhead > 0);
}

void
FDIP::setBranchPredictor(branch_prediction::BPredUnit *bp)
{
    branchPred = bp;
}

std::vector<Addr>
FDIP::predictBranchTargets(Addr pc, unsigned numBranches)
{
    std::vector<Addr> targets;
    
    if (!branchPred) {
        DPRINTF(FDIP, "Branch predictor not set, cannot predict targets\n");
        return targets;
    }
    
    Addr currentPC = pc;
    
    // Create a temporary PCState for branch prediction
    std::unique_ptr<PCStateBase> pcState(branchPred->getInstPC(currentPC));
    
    // Predict up to numBranches branches
    for (unsigned i = 0; i < numBranches; i++) {
        void *bp_history = nullptr;
        
        // Use the branch predictor to predict the next branch
        bool taken = branchPred->lookup(threadId, currentPC, bp_history);
        
        if (taken) {
            // Get the predicted target
            Addr target = branchPred->getTargetAddr(currentPC, bp_history);
            targets.push_back(target);
            
            // Update the current PC to the predicted target
            currentPC = target;
        } else {
            // If not taken, assume sequential execution (next instruction)
            // For simplicity, we'll just increment by 4 (typical instruction size)
            currentPC += 4;
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
}

void
FDIP::calculatePrefetch(const PrefetchInfo &pfi,
                       std::vector<AddrPriority> &addresses,
                       const CacheAccessor &cache)
{
    // Clean up old prefetched addresses
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
    
    // Predict branch targets
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
    
    // Prefetch based on predicted targets
    unsigned prefetched = 0;
    
    for (Addr target : targets) {
        // Convert target address to cache line address
        Addr target_line = blockAddress(target);
        
        // Check if this line is already in the cache or recently prefetched
        if (cache.inCache(target_line, pfi.isSecure()) || 
            cache.inMissQueue(target_line, pfi.isSecure()) ||
            isRecentlyPrefetched(target_line)) {
            continue;
        }
        
        // Add to prefetch queue with decreasing priority
        addresses.push_back(AddrPriority(target_line, prefetched));
        markPrefetched(target_line);
        
        DPRINTF(FDIP, "FDIP prefetch: 0x%x (from target 0x%x)\n", 
                target_line, target);
        
        prefetched++;
        
        // Stop if we've reached the degree limit
        if (prefetched >= degree) {
            break;
        }
    }
}

} // namespace prefetch
} // namespace gem5