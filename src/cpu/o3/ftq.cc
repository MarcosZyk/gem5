#include "cpu/o3/ftq.hh"

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "debug/FTQ.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{


/** Fetch Target Methods -------------------------------- */
FetchTarget::FetchTarget(const PCStateBase &start_pc, InstSeqNum seq_num)
    : ftSeqNum(seq_num),
      end_with_branch(false), taken(false),
      bpu_history(nullptr)
{
    set(startPC , start_pc);
}


void
FetchTarget::finalize(const PCStateBase &exit_pc, InstSeqNum sn,
                      bool _is_branch, bool pred_taken,
                      const PCStateBase &pred_pc)
{
    set(endPC, exit_pc);
    set(predPC, pred_pc);
    taken = pred_taken;
    end_with_branch = _is_branch;
}


/** Fetch Target Qeue Methods ----------------------------- */

FetchTargetQueue::FetchTargetQueue(CPU *belonged_cpu, const BaseO3CPUParams &params)
    : cpu(belonged_cpu),
      numThreads(params.numThreads),
      numEntries(params.numFTQEntries),
      statsFTQ(belonged_cpu, this)
{
    resetState();
}


void
FetchTargetQueue::resetState()
{
    for (ThreadID tid = 0; tid  < numThreads; tid++) {
        ftq[tid].clear();
        ftqStatus[tid] = Valid;
    }
}


std::string
FetchTargetQueue::name() const
{
    return cpu->name() + ".ftq";
}


void
FetchTargetQueue::regProbePoints()
{
    ppFTQInsert = new ProbePointArg<FetchTargetPtr>(cpu->getProbeManager(),
                                                        "FTQInsert");
    ppFTQRemove = new ProbePointArg<FetchTargetPtr>(cpu->getProbeManager(),
                                                        "FTQRemove");
}

unsigned
FetchTargetQueue::numFreeEntries(ThreadID tid)
{
    return numEntries - ftq[tid].size();
}

unsigned
FetchTargetQueue::size(ThreadID tid)
{
    return ftq[tid].size();
}

bool
FetchTargetQueue::isFull(ThreadID tid)
{
    return ftq[tid].size() >= numEntries;
}

bool
FetchTargetQueue::isEmpty() const
{
    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        if (!ftq[tid].empty()) return false;
    }
    return true;
}

bool
FetchTargetQueue::isEmpty(ThreadID tid) const
{
    return ftq[tid].empty();
}


void
FetchTargetQueue::invalidate(ThreadID tid)
{
    /** Only a full ftq can be invalid*/
    if (!ftq[tid].empty())
        ftqStatus[tid] = Invalid;
}

bool
FetchTargetQueue::isValid(ThreadID tid)
{
    return ftqStatus[tid] != Invalid;
}


void
FetchTargetQueue::lock(ThreadID tid)
{
    ftqStatus[tid] = Locked;
}

bool
FetchTargetQueue::isLocked(ThreadID tid)
{
    return ftqStatus[tid] == Locked;
}


void
FetchTargetQueue::forAllForward(ThreadID tid, std::function<void(FetchTargetPtr&)> f)
{
    for (auto it = ftq[tid].begin(); it != ftq[tid].end(); it++) {
        f(*it);
    }
}

void
FetchTargetQueue::forAllBackward(ThreadID tid, std::function<void(FetchTargetPtr&)> f)
{
    for (auto it = ftq[tid].rbegin(); it != ftq[tid].rend(); it++) {
        f(*it);
    }
}



void
FetchTargetQueue::insert(ThreadID tid, FetchTargetPtr fetchTarget)
{
    ftq[tid].push_back(fetchTarget);
    ppFTQInsert->notify(fetchTarget);
    statsFTQ.inserts++;
    statsFTQ.occupancy.sample(ftq[tid].size());

    DPRINTF(FTQ, "Insert in FTQ[T:%i]. size FTQ:%i\n",
                    tid, ftq[tid].size());
}


void
FetchTargetQueue::squash(ThreadID tid)
{
    for (auto ft : ftq[tid]) {
        assert(ft->bpu_history == nullptr);
        ppFTQRemove->notify(ft);
    }
    ftq[tid].clear();
    ftqStatus[tid] = Valid;
    statsFTQ.squashes++;
}

void
FetchTargetQueue::squashSanityCheck(ThreadID tid)
{
    for (auto ft : ftq[tid]) {
        assert(ft->bpu_history == nullptr);
    }
}


bool
FetchTargetQueue::isHeadReady(ThreadID tid)
{
    return (ftqStatus[tid] != Invalid) && (ftq[tid].size() > 0);
}


FetchTargetPtr
FetchTargetQueue::readHead(ThreadID tid)
{
    if (ftqStatus[tid] == Invalid) return nullptr;
    if (ftq[tid].empty()) return nullptr;

    return ftq[tid].front();
}


bool
FetchTargetQueue::updateHead(ThreadID tid)
{
    if (ftq[tid].front()->bpu_history != nullptr) {
        DPRINTF(FTQ, "Pop FT:[fn%llu] failed. Still contains BP history.\n",
                    ftq[tid].front()->ftNum());
        ftqStatus[tid] = Invalid;
        return false;
    }

    bool ret_val = true;

    // TODO make this more efficient
    // Once the head of the FTQ gets updated and
    // the FTQ got blocked by a complex instruction resteere
    // we unblock by squashing
    if (ftqStatus[tid] == Locked) {
        DPRINTF(FTQ, "Pop FT:[fn%llu] unblocks FTQ. Require squash.\n",
                    ftq[tid].front()->ftNum());
        ftqStatus[tid] = Invalid;
        ret_val = false;
    }

    ppFTQRemove->notify(ftq[tid].front());
    ftq[tid].pop_front();
    statsFTQ.removals++;
    return ret_val;
}



FetchTargetQueue::FTQStats::FTQStats(o3::CPU *cpu, FetchTargetQueue *ftq)
  : statistics::Group(cpu, "ftq"),
    ADD_STAT(inserts, statistics::units::Count::get(),
        "The number of FTQ insertions"),
    ADD_STAT(removals, statistics::units::Count::get(),
        "The number of FTQ removals. Not including squashes"),
    ADD_STAT(squashes, statistics::units::Count::get(),
        "The number of FTQ squashes"),
    ADD_STAT(locks, statistics::units::Count::get(),
        "The number of times the FTQ got locked."),
    ADD_STAT(occupancy, statistics::units::Count::get(),
        "Distribution of the FTQ occupation.")
{
    occupancy
        .init(/* base value */ 0,
              /* last value */ ftq->numEntries,
              /* bucket size */ 4)
        .flags(statistics::pdf);
}

} // namespace o3
} // namespace gem5
