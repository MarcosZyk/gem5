#include "mem/cache/prefetch/fetch_directed.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FetchDirected::FetchDirected(const FetchDirectedPrefetcherParams &p)
    : Base(p),
      maxStreams(p.max_streams),
      degree(p.degree),
      crossPages(p.cross_pages),
      streams(maxStreams),
      streamIndex(0)
{
}

void
FetchDirected::notifyBranchPrediction(Addr pc, Addr target, bool taken)
{
    if (!taken) {
        // Only prefetch for taken branches
        return;
    }

    DPRINTF(HWPrefetch, "Branch prediction: PC %#x -> target %#x (taken)\n",
            pc, target);

    // Find an unused stream or the oldest allocated one
    unsigned idx = 0;
    for (unsigned i = 0; i < maxStreams; i++) {
        if (!streams[i].valid) {
            idx = i;
            break;
        }
        if (streams[i].startAddr == target) {
            // Already tracking this target
            return;
        }
        if (i > idx && streams[i].valid) {
            idx = i;
        }
    }

    // Allocate a new stream
    streams[idx].valid = true;
    streams[idx].startAddr = target;
    streams[idx].nextAddr = target;
    streams[idx].fromBranchPred = true;

    DPRINTF(HWPrefetch, "Allocated branch prediction stream %u: %#x\n",
            idx, target);

    // Immediately issue prefetches for this stream
    std::vector<AddrPriority> addresses;
    Addr curr_addr = streams[idx].nextAddr;
    Addr blk_addr = blockAddress(curr_addr);

    for (int d = 0; d < degree; d++) {
        Addr pf_addr = blk_addr + d * blkSize;

        if (!crossPages && !samePage(curr_addr, pf_addr)) {
            // Stop if we've crossed a page boundary and are not allowed to
            break;
        }

        DPRINTF(HWPrefetch, "Branch prediction prefetch: %#x\n", pf_addr);
        addresses.push_back(AddrPriority(pf_addr, 0));
    }

    // Update the stream's next address
    streams[idx].nextAddr = blk_addr + degree * blkSize;

    // Queue up the prefetches
    for (const auto& addr_prio : addresses) {
        PrefetchInfo pfi(addr_prio.first, addr_prio.second, requestorId);
        if (useVirtualAddresses) {
            // Generate a prefetch to this address
            Queued::SepPrefetchReq *pf_req =
                new Queued::SepPrefetchReq(this, pfi, pfi.getTarget(), 
                                          pfi.getPriority());
            cpuSidePort.sendFunctionalPacket(pf_req);
            delete pf_req;
        } else {
            // Generate a prefetch to this address
            Queued::SepPrefetchReq *pf_req =
                new Queued::SepPrefetchReq(this, pfi, pfi.getPaddr(), 
                                          pfi.getPriority());
            cpuSidePort.sendFunctionalPacket(pf_req);
            delete pf_req;
        }
    }
}

void
FetchDirected::calculatePrefetch(const PrefetchInfo &pfi,
                                std::vector<AddrPriority> &addresses)
{
    // This is called on cache misses
    Addr pf_addr = pfi.getAddr();
    bool is_secure = pfi.isSecure();

    DPRINTF(HWPrefetch, "Calculate prefetch for %#x\n", pf_addr);

    // Check if this address is part of any existing stream
    for (unsigned i = 0; i < maxStreams; i++) {
        if (!streams[i].valid) {
            continue;
        }

        // If this address is in an existing stream, prefetch the next blocks
        if (streams[i].startAddr <= pf_addr &&
            pf_addr < streams[i].nextAddr) {
            DPRINTF(HWPrefetch, "Hit in stream %u: %#x-%#x\n",
                    i, streams[i].startAddr, streams[i].nextAddr);

            Addr curr_addr = streams[i].nextAddr;
            Addr blk_addr = blockAddress(curr_addr);

            for (int d = 0; d < degree; d++) {
                Addr new_pf_addr = blk_addr + d * blkSize;

                if (!crossPages && !samePage(curr_addr, new_pf_addr)) {
                    // Stop if we've crossed a page boundary and are not allowed to
                    break;
                }

                DPRINTF(HWPrefetch, "Stream prefetch: %#x\n", new_pf_addr);
                addresses.push_back(AddrPriority(new_pf_addr, 0));
            }

            // Update the stream's next address
            streams[i].nextAddr = blk_addr + degree * blkSize;
            return;
        }
    }

    // If we get here, this address wasn't part of any existing stream
    // Allocate a new stream if this is an instruction fetch
    if (pfi.isInstFetch()) {
        // Find an unused stream or the oldest allocated one
        unsigned idx = streamIndex;
        streamIndex = (streamIndex + 1) % maxStreams;

        // Allocate a new stream
        streams[idx].valid = true;
        streams[idx].startAddr = pf_addr;
        streams[idx].nextAddr = blockAddress(pf_addr) + blkSize;
        streams[idx].fromBranchPred = false;

        DPRINTF(HWPrefetch, "Allocated instruction stream %u: %#x\n",
                idx, pf_addr);

        // Prefetch the next few blocks
        Addr curr_addr = streams[idx].nextAddr;
        Addr blk_addr = blockAddress(curr_addr);

        for (int d = 0; d < degree; d++) {
            Addr new_pf_addr = blk_addr + d * blkSize;

            if (!crossPages && !samePage(curr_addr, new_pf_addr)) {
                // Stop if we've crossed a page boundary and are not allowed to
                break;
            }

            DPRINTF(HWPrefetch, "New stream prefetch: %#x\n", new_pf_addr);
            addresses.push_back(AddrPriority(new_pf_addr, 0));
        }

        // Update the stream's next address
        streams[idx].nextAddr = blk_addr + degree * blkSize;
    }
}

} // namespace prefetch
} // namespace gem5