#ifndef __CPU_O3_FTQ_HH__
#define __CPU_O3_FTQ_HH__

#include <list>
#include <string>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/limits.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

struct DerivO3CPUParams;

// Dynamic instruction execution in O3 CPU.
// One instruction may be re-executed, thus in need of a runtime ID.
typedef InstSeqNum FTSeqNum;


class FetchTarget
{
  public:
    FetchTarget(const PCStateBase &start_pc, FTSeqNum seq_num);

  private:

    /** Sequence number/runtime id */
    const FTSeqNum ftSeqNum;

    /** Instruction fetch/prefetch is based on instruction block.
     * We fetch consecutive instructions in one access.
     */
    std::unique_ptr<PCStateBase> startPC;
    std::unique_ptr<PCStateBase> endPC;

    /** instruction type of last one in this target, normal or branch */
    bool end_with_branch;

    /** The predicted address of next possible fetch target.
     * Only works when this target ends with a branch*/
    std::unique_ptr<PCStateBase> predPC;

    /** debug usage, whether the branch, last instruction, is taken */
    bool taken;

  public:
    /** branch predictor history information while FT is waiting in th FTQ. */
    void* bpu_history;

    Addr startAddress() { return startPC->instAddr(); }

    Addr endAddress() { return (endPC) ? endPC->instAddr() : MaxAddr; }

    unsigned size() { return endAddress() - startAddress(); }

    bool inRange(Addr addr) {
        return addr >= startAddress() && addr <= endAddress();
    }

    bool isEndInst(Addr addr) {
        return addr == endAddress();
    }

    /** check whether the inst of given address is the last one of this target and is a branch*/
    bool isEndBranch(Addr addr) {
        return (addr == endAddress()) && end_with_branch;
    }

    bool hasExceeded(Addr addr) {
        return addr > endAddress();
    }

    FTSeqNum getFetchSeqNum() { return ftSeqNum; }

    const PCStateBase &getPredictedTarget() { return *predPC; }

    bool isTaken() { return taken; }

    /** Complete a fetch target with the end instruction */
    void sealTarget(const PCStateBase &exit_pc, InstSeqNum sn, bool _is_branch,
                  bool pred_taken, const PCStateBase &pred_pc);
};


typedef std::shared_ptr<FetchTarget> FetchTargetPtr;



class FetchTargetQueue
{
  public:
    FetchTargetQueue(CPU *belonged_cpu, const BaseO3CPUParams &params);

    std::string name() const;


  private:

    enum Status
    {
        Invalid,
        Valid,
        Full,
        Locked
    };

    /** Per-thread FTQ status. */
    Status ftqStatus[MaxThreads];

    CPU *cpu;

    const unsigned maxThreadNum;

    /** (per thread) Number of fetch targets in the FTQ. */
    const unsigned numEntries;

    /** Probe points to attach the FDP perfetcher. */
    ProbePointArg<FetchTargetPtr> *ppFTQInsert;
    ProbePointArg<FetchTargetPtr> *ppFTQRemove;

    /** FTQ List of Fetch targets */
    std::list<FetchTargetPtr> ftq[MaxThreads];



public:

    /** Registers probes. */
    void regProbePoints();

    /** Reset the FTQ state */
    void resetState();


    unsigned numFreeEntries(ThreadID tid);

    unsigned size(ThreadID tid);

    bool isFull(ThreadID tid);

    bool isEmpty(ThreadID tid) const;


    /** Invalidates all fetch targets in the FTQ.
     * Requires squash to recover. */
    void invalidate(ThreadID tid);

    /** Returns if the FTQ is in a valid state*/
    bool isValid(ThreadID tid);

    void insert(ThreadID tid, FetchTargetPtr fetchTarget);

    bool isHeadReady(ThreadID tid);

    FetchTargetPtr readHead(ThreadID tid);

    bool updateHead(ThreadID tid);

    /** Iterates backward over all fetch targets in the FTQ from tail/back to
     * head/front and applies a given function.
     * Used for PFC
     */
    void forAllBackward(ThreadID tid, std::function<void(FetchTargetPtr&)> f);

    /** Squashes all fetch targets in the FTQ for a specific thread. */
    void squash(ThreadID tid);

    /***/
    void squashSanityCheck(ThreadID tid);

    /** Locks the fetch target queue for a given thread. Locking is different
     * from invalidating in that the head/front fetch targets are still
     * valid and accessible. However, all other FTs are invalid and the
     * FTQ must be squashed to recover. */
    void lock(ThreadID tid);

    /** Check if the FTQ is locked. */
    bool isLocked(ThreadID tid);


  private:

    struct FTQStats : public statistics::Group
    {
        FTQStats(CPU *cpu, FetchTargetQueue *ftq);

        statistics::Scalar inserts;
        statistics::Scalar removals;
        statistics::Scalar squashes;
        statistics::Scalar locks;

        statistics::Distribution occupancy;
    } statsFTQ;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_FTQ_HH__
