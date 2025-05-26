#ifndef __CPU_O3_FTG_HH__
#define __CPU_O3_FTG_HH__

#include <list>

#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/timebuf.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class FetchTargetQueue;
class FetchTarget;
typedef std::shared_ptr<FetchTarget> FetchTargetPtr;


/**
 * Refer to fetch stage to implement this stage by extracting branch prediction and next PC address
 * calculation from fetch stage.
*/
class FTG
{
  typedef branch_prediction::BranchType BranchType;

  public:

    enum FTGStatus
    {
        Active,
        Inactive
    };

    enum ThreadStatus
    {
        Idle,
        Running,
        Squashing,
        Blocked,
        FTQFull,
        FTQLocked
    };

  private:

    FTGStatus _status;

    CPU* cpu;


    /** Time buffer interface. */
    TimeBuffer<TimeStruct> *timeBuffer;
    TimeBuffer<TimeStruct>::wire fromFetch;
    TimeBuffer<TimeStruct>::wire fromDecode;
    TimeBuffer<TimeStruct>::wire fromCommit;
    TimeBuffer<FetchStruct>::wire toFetch;


    /** Variable that tracks if FTG has written to the time buffer this
     * cycle. Used to tell CPU if there is activity this cycle.
     */
    bool wroteToTimeBuffer;

    /** Source of possible stalls. */
    struct Stalls
    {
      bool fetch;
      bool drain;
      bool bpu;
    };

    /** Tracks which stages are telling the ftq to stall. */
    Stalls stalls[MaxThreads];
    /** Fetch to FTG delay. */
    const Cycles fetchToFTGDelay;
    /** Decode to fetch delay. (Same delay for FTG as for fetch) */
    const Cycles decodeToFetchDelay;
    /** Commit to fetch delay. (Same delay for FTG as for fetch) */
    const Cycles commitToFetchDelay;
    /** FTG to fetch delay. */
    const Cycles ftgToFetchDelay;


    /** Per-thread status. */
    ThreadStatus ftgStatus[MaxThreads];
    /** Per-thread PC */
    std::unique_ptr<PCStateBase> ftgPC[MaxThreads];
    /** List of Active FTQ Threads */
    std::list<ThreadID> *activeThreads;
    /** Number of threads. */
    const ThreadID numThreads;


    branch_prediction::BPredUnit* bpu;
    FetchTargetQueue* ftq;
    /** The maximum width of a fetch target. This also determines the
     * maximum addresses searched in one cycle. */
    const unsigned fetchTargetWidth;



  protected:
    struct FTGStats : public statistics::Group
    {
      FTGStats(CPU *cpu, FTG *ftg);

      /** Stat for total number of idle cycles. */
      statistics::Scalar idleCycles;
      /** Stat for total number of normal running cycles. */
      statistics::Scalar runCycles;
      /** Stat for total number of squashing cycles. */
      statistics::Scalar squashCycles;
      /** Stat for total number of cycles the FTQ was full. */
      statistics::Scalar ftqFullCycles;

      /** Stat for total number fetch targets created. */
      statistics::Scalar fetchTargets;
      /** Total number of branches detected. */
      statistics::Scalar branches;
      /** Total number of branches predicted taken. */
      statistics::Scalar predTakenBranches;
      /** Total number of fetched branches. */
      statistics::Scalar branchesNotLastuOp;

      /** Stat for total number of misspredicted instructions. */
      statistics::Scalar branchMisspredict;
      statistics::Scalar noBranchMisspredict;

      /** Stat for total number of misspredicted instructions. */
      statistics::Scalar squashBranchDecode;
      statistics::Scalar squashBranchCommit;

      /** Number of post updates */
      statistics::Vector preDecUpdate;
      /** Number of branches undetected by the BPU */
      statistics::Vector noHistType;

      /** Stat for the two corner cases */
      statistics::Scalar typeMissmatch;
      statistics::Scalar multiBranchInst;

      /** Distribution of number of bytes per fetch target. */
      statistics::Distribution ftSizeDist;

    } statsFTG;


  public:

    FTG(CPU *_cpu, const BaseO3CPUParams &params);

    std::string name() const;

    void regProbePoints() {}
    void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr);
    void setActiveThreads(std::list<ThreadID> *at_ptr);
    void startupStage();
    void clearStates(ThreadID tid);
    void drainResume();
    void drainSanityCheck() const;
    bool isDrained() const;
    void drainStall(ThreadID tid);
    void takeOverFrom() { resetStage(); }
    void deactivateThread(ThreadID tid) {};
    void tick();

    void setFetchTargetQueue(FetchTargetQueue * _ptr);

    /**
     * Calculate the next PC address depending on the instruction and the branch prediction.
     * @param inst The currently processed dynamic instruction.
     * @param fetch_pc The current fetch PC passed in by reference. It will
     * be updated with what the next PC will be.
     * @param ft The currently processed fetch target. Can be nullptr for
     * the non-decoupled scenario.
     * @return Whether or not a branch was predicted as taken.
     */
    bool updatePC(const DynInstPtr &inst, PCStateBase &fetch_pc,
                  FetchTargetPtr &ft);

  private:

    void resetStage();
    void switchToActive();
    void switchToInactive();
    bool checkStall(ThreadID tid) const;
    void updateFTGStatus();

    /**
     * Checks all input signals and updates the status as necessary.
     * PFC implemented in this method.
     */
    bool checkSignalsAndUpdate(ThreadID tid);

    /** Check the backward signals that update the BPU. */
    bool checkAndUpdateBPUSignals(ThreadID tid);


    /**
     * Create a new fetch target.
     * @param start_pc The current PC. Will be the start address of the
     * fetch target.
    */
    FetchTargetPtr newFetchTarget(ThreadID tid, const PCStateBase &start_pc);

    /**
     * The prediction function for the FTG stage.
     *
     * @param inst The branch instruction.
     * @param ft The fetch target that is currently processed.
     * @param PC The predicted PC is passed back through this parameter.
     * @return Returns if the branch is taken or not.
     */
    bool predict(ThreadID tid, const StaticInstPtr &inst,
                 const FetchTargetPtr &ft, PCStateBase &pc);


    /**
     * Main function that feeds the FTQ with new fetch targets.
     *
     * By leveraging the BTB, up to N consecutive addresses are searched to detect a branch instruction.
     *
     * For every BTB hit the direction, predictor is asked to make a prediction.
     *
     * In every cycle one fetch target is created. A fetch target ends once the first branch instruction is detected
     * or the maximum earch bandwidth for a cycle is reached.
     **/
    void generateFetchTargets(ThreadID tid, bool &status_change);


    /**
     * After pre-decoding instruction in the fetch stage all instructions
     * are known together and a sequence number is assigned to them.
     * The fetch stage will call this function for every branch instruction
     * to allow the FTG stage to update the branch predictor history.
     */
    bool updatePreDecode(ThreadID tid, const InstSeqNum seqNum,
                         const StaticInstPtr &inst, PCStateBase &pc,
                         const FetchTargetPtr &ft);

    /** Squashes FTG for a specific thread and resets the PC. */
    void squash(const PCStateBase &new_pc, ThreadID tid);

    /**
     * Squashes the BPU histories in the FTQ.
     * by iterating from tail to head and reverts the predictions made.
     **/
    void squashBpuHistories(ThreadID tid);

    /** Update the stats per cycle */
    void profileCycle(ThreadID tid);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FTG_HH__
