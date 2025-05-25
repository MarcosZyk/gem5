/**
 * Implementation of the fetch directed instruction prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_FDP_HH__
#define __MEM_CACHE_PREFETCH_FDP_HH__


#include <list>

#include "cpu/base.hh"
#include "cpu/o3/ftq.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct FetchDirectedInstructionPrefetcherParams;

namespace prefetch
{

class FetchDirectedInstructionPrefetcher : public Base
{

  public:
    FetchDirectedInstructionPrefetcher(const FetchDirectedInstructionPrefetcherParams &p);
    ~FetchDirectedInstructionPrefetcher() = default;

    void regProbeListeners() override;

    void setCache(BaseCache *belonged_cache) override { cache = belonged_cache; }

    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override
    {
        return prefetchQueue.empty() ? MaxTick : prefetchQueue.front().readyTime;
    }

  private:

    /** Listening events */
    std::vector<ProbeListener *> listeners;

    /** Use its probeManager to register listener */
    BaseCPU *cpu;

    /** Used to check block existence */
    BaseCache *cache;

    /** The latency of prefetch operation */
    const unsigned int latency;

    struct PFQEntry
    {
        PFQEntry(uint64_t addr, PacketPtr p, Tick t)
            : targetAddress(addr), cacheReqPackage(p), readyTime(t) {}

        uint64_t targetAddress;

        PacketPtr cacheReqPackage;

        /** Send cacheReqPackage to cache at this time. */
        Tick readyTime;

        bool operator==(const int& that_address) const {
            return this->targetAddress == that_address;
        }
    };

    std::list<PFQEntry> prefetchQueue;

    void
    notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
    override
    {}

    /** When a new fetch target is inserted into the FTQ, sync it to PFQ. */
    void notifyFTQInsert(const o3::FetchTargetPtr& fetch_target);

    PacketPtr createPrefetchPacket(Addr block_address);

    /** Current implementation directly prefetch from memory. Therefore, translation is needed. */
    bool translateVirtualAddress(RequestPtr req);


    // referred to queued.hh
  protected:
    struct FDIPStats : public statistics::Group
    {
        FDIPStats(statistics::Group *parent);

        statistics::Scalar pfIdentified;
        statistics::Scalar pfInCache;
        statistics::Scalar pfInCachePrefetched;
        statistics::Scalar pfPacketsCreated;
        statistics::Scalar pfCandidatesAdded;

    } statsFDIP;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FDP_HH__
