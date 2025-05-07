#ifndef __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__
#define __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__

#include <deque>

#include "base/sat_counter.hh"
#include "mem/cache/prefetch/base.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

/**
 * Fetch-Directed Instruction Prefetching
 * This prefetcher works by prefetching instruction cache lines based on
 * branch prediction information from the CPU's fetch stage.
 * It prefetches the target of predicted taken branches to reduce instruction
 * cache misses.
 */
class FetchDirected : public Base
{
  protected:
    /** Max number of prefetch streams */
    const unsigned maxStreams;

    /** Number of prefetch blocks to fetch */
    const unsigned degree;

    /** Whether to prefetch across page boundaries */
    const bool crossPages;

    /** Structure to track prefetch streams */
    struct Stream
    {
        /** Starting address of the stream */
        Addr startAddr;

        /** Next address to prefetch in the stream */
        Addr nextAddr;

        /** Whether this stream is valid */
        bool valid;

        /** Whether this stream was triggered by a branch prediction */
        bool fromBranchPred;

        /** Constructor */
        Stream() : startAddr(0), nextAddr(0), valid(false), fromBranchPred(false) {}
    };

    /** Array of prefetch streams */
    std::vector<Stream> streams;

    /** Current index in the streams array */
    unsigned streamIndex;

  public:
    FetchDirected(const FetchDirectedPrefetcherParams &p);
    ~FetchDirected() = default;

    /**
     * Called when there is a branch prediction event from the CPU.
     * This method is called by the CPU's fetch stage when a branch is predicted.
     * @param pc The PC of the branch instruction
     * @param target The target address of the branch
     * @param taken Whether the branch is predicted taken
     */
    void notifyBranchPrediction(Addr pc, Addr target, bool taken);

    /**
     * Calculate the next prefetch address based on the current address.
     * @param pf_addr Current address
     * @param is_secure Whether the address is secure
     * @param master_id The ID of the master requesting the prefetch
     */
    void calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FETCH_DIRECTED_HH__