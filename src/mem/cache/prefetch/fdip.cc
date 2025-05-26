#include "mem/cache/prefetch/fdip.hh"

#include <utility>

#include "debug/FDIP.hh"
#include "mem/cache/base.hh"
#include "params/FetchDirectedInstructionPrefetcher.hh"

namespace gem5
{

namespace prefetch
{




FetchDirectedInstructionPrefetcher::FetchDirectedInstructionPrefetcher(
                                const FetchDirectedInstructionPrefetcherParams &p)
    : Base(p),
      cpu(p.cpu),
      cache(nullptr),
      latency(cyclesToTicks(p.latency)),
      statsFDIP(this)
{
}


FetchDirectedInstructionPrefetcher::FDIPStats::FDIPStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(pfIdentified, statistics::units::Count::get(),
                "number of prefetches identified."),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
                "number of prefetches hit in in cache"),
    ADD_STAT(pfInCachePrefetched, statistics::units::Count::get(),
                "number of prefetches hit in cache but prefetched"),
    ADD_STAT(pfPacketsCreated, statistics::units::Count::get(),
                "number of prefetch packets created"),
    ADD_STAT(pfCandidatesAdded, statistics::units::Count::get(),
                "Number of perfetch candidates added to the prefetch queue")
{
}

void
FetchDirectedInstructionPrefetcher::notifyFTQInsert(const o3::FetchTargetPtr& fetch_target)
{
    Addr block_address = blockAddress(fetch_target->startAddress());

    // filter out req with existing address in the prefetch queue
    std::list<PFQEntry>::iterator iterator = std::find(prefetchQueue.begin(), prefetchQueue.end(), block_address);
    if (iterator == prefetchQueue.end()) {
        statsFDIP.pfIdentified++;
    } else {
        DPRINTF(FDIP, "%#x already exists in prefetch_queue\n", block_address);
        return;
    }

    PacketPtr prefetch_packet = createPrefetchPacket(block_address);
    if (prefetch_packet) {
        statsFDIP.pfPacketsCreated++;
    } else {
        DPRINTF(FDIP, "Fail to create packet\n");
        delete prefetch_packet;
        return;
    }

    if ((cache->inCache(prefetch_packet->getAddr(), prefetch_packet->isSecure())
                || (cache->inMissQueue(prefetch_packet->getAddr(), prefetch_packet->isSecure())))) {
        statsFDIP.pfInCache++;
        DPRINTF(FDIP, "%#x already exists in cache/mshr\n", block_address);
        delete prefetch_packet;
        return;
    }

    Tick t = curTick() + latency;
    prefetchQueue.push_back(PFQEntry(block_address, prefetch_packet, t));
    statsFDIP.pfCandidatesAdded++;
}


PacketPtr
FetchDirectedInstructionPrefetcher::createPrefetchPacket(Addr block_address)
{
    /* Packet is based on mem request */
    Flags flags = Request::INST_FETCH | Request::PREFETCH;
    RequestPtr req = std::make_shared<Request>(block_address, blkSize, flags, requestorId, block_address, 0);


    // CPU pipeline works based on virtual addresses, which is in FTQ.
    if (!translateVirtualAddress(req)) {
        return nullptr;
    }

    if (req->isUncacheable()) {
        return nullptr;
    }

    req->taskId(context_switch_task_id::Prefetcher);

    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();

    return pkt;
}


bool
FetchDirectedInstructionPrefetcher::translateVirtualAddress(RequestPtr req)
{
    if (mmu == nullptr) {
        return false;
    }

    auto thread_context = cache->system->threads[req->contextId()];

    Fault fault = mmu->translateFunctional(req, thread_context, BaseMMU::Read);
    return fault == NoFault;
}


void
FetchDirectedInstructionPrefetcher::regProbeListeners()
{
    Base::regProbeListeners();

    if (cpu == nullptr) {
        warn("No CPU to listen from registered\n");
        return;
    }
    typedef ProbeListenerArgFunc<o3::FetchTargetPtr> FetchTargetListener;
    listeners.push_back(
            new FetchTargetListener(cpu->getProbeManager(), "FTQInsert",
                [this](const o3::FetchTargetPtr &ft)
                    { notifyFTQInsert(ft); }));

    // todo implement sync for FTQ Removal; currently, waste the prefetch

}


PacketPtr
FetchDirectedInstructionPrefetcher::getPacket()
{
    // This function is used for prefetch issuing.
    if (prefetchQueue.size() == 0)
    {
        return nullptr;
    }
    PacketPtr pkt = prefetchQueue.front().cacheReqPackage;

    DPRINTF(FDIP, "Issue Prefetch pkt:%#x at PC:%#x\n", pkt->getAddr(), prefetchQueue.front().targetAddress);

    prefetchQueue.pop_front();

    prefetchStats.pfIssued++;
    issuedPrefetches++;

    return pkt;
}

} // namespace prefetch
} // namespace gem5
