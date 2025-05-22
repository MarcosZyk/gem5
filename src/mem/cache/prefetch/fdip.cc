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
      maxStreams(p.max_streams)
{
    // Ensure parameters are valid
    assert(degree > 0);
    assert(lineSize > 0);
    assert(maxLookAhead > 0);
    assert(confidenceThreshold <= 100);
    assert(maxStreams > 0);
    
    // Initialize branch streams
    branchStreams.resize(maxStreams);
    
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

std::vector<Addr>
FDIP::predictBranchTargets(Addr pc, unsigned numBranches)
{
    std::vector<Addr> targets;
    
    if (!branchPred) {
        DPRINTF(FDIP, "Branch predictor not set, cannot predict targets\n");
        return targets;
    }
    
    // First check if we have a stream for this PC
    BranchStream* stream = findOrCreateStream(pc);
    if (stream->valid && !stream->targets.empty()) {
        // Use the existing stream
        DPRINTF(FDIP, "Using existing branch stream for PC 0x%x\n", pc);
        stream->lastUsed = curTick();
        return stream->targets;
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
    
    // Update the stream with the new targets
    if (!targets.empty()) {
        stream->targets = targets;
        stream->lastUsed = curTick();
        stream->valid = true;
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
    
    // Also clean up old branch streams
    for (auto& stream : branchStreams) {
        if (stream.valid && currentTick - stream.lastUsed > cleanupInterval) {
            stream.valid = false;
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

FDIP::BranchStream*
FDIP::findOrCreateStream(Addr pc)
{
    // First look for an existing stream with this PC
    for (auto& stream : branchStreams) {
        if (stream.valid && stream.startPC == pc) {
            return &stream;
        }
    }
    
    // No existing stream, find an invalid one or the oldest one
    BranchStream* oldestStream = &branchStreams[0];
    for (auto& stream : branchStreams) {
        if (!stream.valid) {
            // Found an invalid stream, use it
            stream.startPC = pc;
            stream.targets.clear();
            stream.lastUsed = curTick();
            stream.valid = true;
            return &stream;
        }
        
        if (stream.lastUsed < oldestStream->lastUsed) {
            oldestStream = &stream;
        }
    }
    
    // All streams are valid, replace the oldest one
    oldestStream->startPC = pc;
    oldestStream->targets.clear();
    oldestStream->lastUsed = curTick();
    oldestStream->valid = true;
    
    return oldestStream;
}

void
FDIP::updateBranchStream(Addr pc, Addr actualTarget)
{
    // Find the stream for this PC
    for (auto& stream : branchStreams) {
        if (stream.valid && stream.startPC == pc) {
            // Update the stream with the actual target
            if (!stream.targets.empty()) {
                // Replace the first target with the actual target
                stream.targets[0] = actualTarget;
            } else {
                // Add the actual target
                stream.targets.push_back(actualTarget);
            }
            stream.lastUsed = curTick();
            return;
        }
    }
    
    // No existing stream, create a new one
    BranchStream* stream = findOrCreateStream(pc);
    stream->targets.clear();
    stream->targets.push_back(actualTarget);
    stream->lastUsed = curTick();
    stream->valid = true;
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
    
    // Update branch stream with actual target
    updateBranchStream(pc, actualTarget);
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