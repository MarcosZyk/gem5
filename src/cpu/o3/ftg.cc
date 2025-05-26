#include "cpu/o3/ftg.hh"

#include <algorithm>

#include "arch/generic/pcstate.hh"
#include "base/trace.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/ftq.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/FTG.hh"
#include "debug/Branch.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3PipeView.hh"
#include "params/BaseO3CPU.hh"

using namespace gem5::branch_prediction;

namespace gem5
{

namespace o3
{

FTG::FTG(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      wroteToTimeBuffer(false),
      fetchToFTGDelay(params.fetchToFTGDelay),
      decodeToFetchDelay(params.decodeToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      ftgToFetchDelay(params.ftgToFetchDelay),
      numThreads(params.numThreads),
      bpu(params.branchPred),
      ftq(nullptr),
      fetchTargetWidth(params.fetchTargetWidth),
      statsFTG(_cpu,this)
{
    fatal_if(fetchTargetWidth < params.fetchBufferSize,
            "Fetch target width should be larger than fetch buffer size!");

    for (int i = 0; i < MaxThreads; i++) {
        ftgPC[i].reset(params.isa[0]->newPCState());
        stalls[i] = {false, false, false};
    }

    assert(bpu!=nullptr);
}

std::string
FTG::name() const {
    return cpu->name() + ".ftg";
}


void
FTG::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromFetch = timeBuffer->getWire(-fetchToFTGDelay);
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
FTG::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
FTG::setFetchTargetQueue(FetchTargetQueue * _ptr)
{
    ftq = _ptr;
}

void
FTG::startupStage()
{
    resetStage();
    switchToActive();
}


void
FTG::resetStage()
{
    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        ftgStatus[tid] = Running;
        set(ftgPC[tid], cpu->pcState(tid));

        stalls[tid].fetch = false;
        stalls[tid].drain = false;
        stalls[tid].bpu = false;
    }

    assert(ftq!=nullptr);
    ftq->resetState();

    wroteToTimeBuffer = false;
    _status = Inactive;
}


void
FTG::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");
        cpu->activateStage(CPU::FTGIdx);
        _status = Active;
    }
}

void
FTG::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");
        cpu->deactivateStage(CPU::FTGIdx);
        _status = Inactive;
    }
}


void
FTG::clearStates(ThreadID tid)
{
    ftgStatus[tid] = Running;
    set(ftgPC[tid], cpu->pcState(tid));

    stalls[tid].fetch = false;
    stalls[tid].drain = false;
    stalls[tid].bpu = false;

    assert(ftq!=nullptr);
    ftq->resetState();
}


void
FTG::tick()
{
    bool activity = false;
    bool status_change = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        // Check stall and squash signals first.
        status_change |= checkSignalsAndUpdate(tid);

        if (ftgStatus[tid] == Running) {
            generateFetchTargets(tid, status_change);
            activity = true;
        }
        profileCycle(tid);
    }

    if (status_change) {
        updateFTGStatus();
    }

    if (activity) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

void
FTG::updateFTGStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (ftgStatus[tid] == Running ||
            ftgStatus[tid] == Squashing) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                cpu->activateStage(CPU::FTGIdx);
            }

            _status = Active;
            return;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FTGIdx);
    }

    _status = Inactive;
}


bool
FTG::checkAndUpdateBPUSignals(ThreadID tid)
{
    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(FTG, "[tid:%i] Squashing from commit. PC = %s\n",
                        tid, *fromCommit->commitInfo[tid].pc);

        // In any case, squash the FTQ and the branch histories in the
        // FTQ first.
        squashBpuHistories(tid);
        squash(*fromCommit->commitInfo[tid].pc, tid);

        // If it was a branch mispredict on a control instruction, update the
        // branch predictor with that instruction, otherwise just kill the
        // invalid state we generated in after sequence number
        if (fromCommit->commitInfo[tid].mispredictInst &&
            fromCommit->commitInfo[tid].mispredictInst->isControl()) {

            bpu->squash(fromCommit->commitInfo[tid].doneSeqNum,
                        *fromCommit->commitInfo[tid].pc,
                        fromCommit->commitInfo[tid].branchTaken, tid, true);
            statsFTG.branchMisspredict++;
            statsFTG.squashBranchCommit++;
        } else {
            bpu->squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
            if (fromCommit->commitInfo[tid].mispredictInst) {
                DPRINTF(FTG, "[tid:%i] Squashing due to mispredict of "
                        "non-control instruction: %s\n",tid,
                        fromCommit->commitInfo[tid]
                            .mispredictInst->staticInst->disassemble(
                                fromCommit->commitInfo[tid]
                            .mispredictInst->pcState().instAddr()));
            } else {
                DPRINTF(FTG, "[tid:%i] Squashing due to "
                "mispredict of non-control instruction: %s\n",tid);
            }
            statsFTG.noBranchMisspredict++;
        }
        return true;

    } else if (fromCommit->commitInfo[tid].doneSeqNum) {
        // Update the branch predictor if it wasn't a squashed instruction
        // that was broadcasted.
        bpu->update(fromCommit->commitInfo[tid].doneSeqNum, tid);
    }

    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing from decode. PC = %s\n",
                        tid, *fromDecode->decodeInfo[tid].nextPC);

        // Squash.
        squashBpuHistories(tid);
        squash(*fromDecode->decodeInfo[tid].nextPC, tid);

        // Update the branch predictor.
        if (fromDecode->decodeInfo[tid].branchMispredict) {

            bpu->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                    *fromDecode->decodeInfo[tid].nextPC,
                    fromDecode->decodeInfo[tid].branchTaken, tid, false);
            statsFTG.branchMisspredict++;
            statsFTG.squashBranchDecode++;
        } else {
            bpu->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                              tid);
            statsFTG.noBranchMisspredict++;
        }
        return true;
    }


    // Check squash signals from fetch.
    if (fromFetch->fetchInfo[tid].squash
        && ftgStatus[tid] != Squashing) {
        DPRINTF(FTG, "Squashing from fetch with PC = %s\n",
                *fromFetch->fetchInfo[tid].nextPC);

        // Squash unless we're already squashing
        squashBpuHistories(tid);
        squash(*fromFetch->fetchInfo[tid].nextPC, tid);
        return true;
    }
    return false;
}


bool
FTG::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is.
    // Check stall signals, block if necessary.
    if (checkAndUpdateBPUSignals(tid)) {
        return true;
    }

    // Check stalls
    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(FTG,"[tid:%i] Drain stall detected.\n",tid);
        // Squash BPU histories and disable the FTQ.
        squashBpuHistories(tid);
        ftq->squash(tid);

        ftgStatus[tid] = Idle;
        return true;
    }

    if (checkStall(tid)) {
        // return block(tid);
        ftgStatus[tid] = Blocked;
        return false;
    }

    // If at this point the FTQ is still invalid we need to wait for
    // A resteer/squash signal.
    if (!ftq->isValid(tid) && ftgStatus[tid] != Idle) {
        DPRINTF(FTG, "[tid:%i] FTQ is invalid. Wait for resteer.\n", tid);

        ftgStatus[tid] = Idle;
        return true;
    }

    // Check if the FTQ got blocked or unblocked
    if ((ftgStatus[tid] == Running) && ftq->isLocked(tid)) {

        DPRINTF(FTG, "[tid:%i] FTQ is locked\n", tid);
        ftgStatus[tid] = FTQLocked;
        return true;
    }
    if ((ftgStatus[tid] == FTQLocked) && !ftq->isLocked(tid)) {

        DPRINTF(FTG, "[tid:%i] FTQ not locked anymore -> Running\n", tid);
        ftgStatus[tid] = Running;
        return true;
    }

    // Check if the FTQ became free in that cycle.
    if ((ftgStatus[tid] == FTQFull) && !ftq->isFull(tid)) {

        DPRINTF(FTG, "[tid:%i] FTQ not full anymore -> Running\n", tid);
        ftgStatus[tid] = Running;
        return true;
    }

    if (ftgStatus[tid] == Squashing) {

        // Switch status to running after squashing FTQ and setting the PC.
        DPRINTF(FTG, "[tid:%i] Done squashing, switching to running.\n", tid);
        ftgStatus[tid] = Running;
        return true;
    }

    // Now all stall/squash conditions are checked.
    // Attempt to run the FTG if not already running.
    if (ftq->isValid(tid) &&
            ((ftgStatus[tid] == Idle) || (ftgStatus[tid] == Blocked))) {

        DPRINTF(FTG, "[tid:%i] Attempt to run\n", tid);
        ftgStatus[tid] = Running;
        return true;
    }

    return false;
}


void
FTG::squashBpuHistories(ThreadID tid)
{

    DPRINTF(FTG, "%s(tid:%i): FTQ sz: %i\n", tid, __func__, ftq->size(tid));

    unsigned n_fts = ftq->size(tid);
    if (n_fts == 0) return;

    // Iterate over the FTQ in reverse order to
    // revert all predictions made.
    ftq->forAllBackward(tid,
        [this, tid](FetchTargetPtr &ft)
        {
            if (ft->bpu_history) {
                auto hist = static_cast<BPredUnit::PredictorHistory*>
                                                    (ft->bpu_history);
                bpu->squashHistory(tid, hist);
                assert(hist == nullptr);
                ft->bpu_history = nullptr;
            }
        });
}

void
FTG::squash(const PCStateBase &new_pc, ThreadID tid)
{
    DPRINTF(FTG, "[tid:%i] Squashing FTQ.\n", tid);
    ftgStatus[tid] = Squashing;
    set(ftgPC[tid], new_pc);
    ftq->squash(tid);
}


FetchTargetPtr
FTG::newFetchTarget(ThreadID tid, const PCStateBase &start_pc)
{
    auto ft = std::make_shared<FetchTarget>(start_pc,
                                            cpu->getAndIncrementFTSeq());

    DPRINTF(FTG, "Create new fetch target ftn:%llu\n", ft->getFetchSeqNum());
    statsFTG.fetchTargets++;
    return ft;
}

bool
FTG::predict(ThreadID tid, const StaticInstPtr &inst,
             const FetchTargetPtr &ft, PCStateBase &pc)
{
    BPredUnit::PredictorHistory* bpu_history = nullptr;
    bool taken  = bpu->predict(inst, ft->getFetchSeqNum(), pc, tid, bpu_history);

    ft->bpu_history = static_cast<void*>(bpu_history);

    DPRINTF(Branch,"[tid:%i, ftn:%llu] History added.\n", tid, ft->getFetchSeqNum());
    return taken;
}


void
FTG::generateFetchTargets(ThreadID tid, bool &status_change)
{

    bool branch_found = false;
    bool predict_taken = false;

    PCStateBase &cur_pc = *ftgPC[tid];
    Addr search_addr = cur_pc.instAddr();
    Addr start_addr = search_addr;

    // In each cycles a new fetch target is created starting with
    // the current PC.
    FetchTargetPtr curFT = newFetchTarget(tid, cur_pc);


    // Scan through the instruction stream and search for branches.
    // The BTB contains only branches where taken at least once.
    while (true) {

        // Check if the current search address can be found in the BTB
        // indicating the end of the branch.
        branch_found = bpu->BTBValid(tid, search_addr);

        if (branch_found) {
            break;
        }

        // If its not a branch check if the maximum search width is reached.
        // If yes stop searching.
        if ((search_addr - start_addr) >= fetchTargetWidth) {
            break;
        }

        search_addr += 1;
    }

    // Update the current PC to point to the last instruction
    // in the fetch target
    cur_pc.set(search_addr);


    // Search stopped either because a branch was found in instruction
    // stream or the maximum search width per cycle was reached.
    // In the first case make the branch prediction and in the later
    // advance the PC to start the search at the following address.

    // Make a copy of the current PC since the BPU will update it.
    std::unique_ptr<PCStateBase> next_pc(cur_pc.clone());
    StaticInstPtr staticInst = nullptr;

    if (branch_found) {
        // Branch found in instruction stream. As the current
        // BPU implementation required the static instruction we need to
        // look it up from the BTB.
        staticInst = bpu->BTBGetInst(tid, cur_pc.instAddr());
        assert(staticInst);

        // Now make the actual prediction. Note the BPU will advance
        // the PC to the next instruction.
        predict_taken = predict(tid, staticInst, curFT, *next_pc);

        DPRINTF(FTG, "[tid:%i, ftn:%llu] Branch found at PC %#x "
                "taken?:%i, target:%#x\n",
                tid, curFT->getFetchSeqNum(), cur_pc.instAddr(),
                predict_taken, next_pc->instAddr());

        statsFTG.branches++;
        if (predict_taken) {
            statsFTG.predTakenBranches++;
        }

    } else {
        // Not a branch therefore continue the next FT at the next address
        next_pc->set(cur_pc.instAddr() + 1);
    }

    curFT->sealTarget(cur_pc, curFT->getFetchSeqNum(), branch_found,
                        predict_taken, *next_pc);

    ftq->insert(tid, curFT);
    wroteToTimeBuffer = true;

    if (ftq->isFull(tid)) {
        DPRINTF(FTG, "FTQ full\n");
        ftgStatus[tid] = FTQFull;
        status_change = true;
    }

    DPRINTF(FTG, "[tid:%i] [fn:%llu] %i addresses searched. "
            "Branch found:%i. Continue with PC:%s in next cycle\n",
            tid, curFT->getFetchSeqNum(), (search_addr - start_addr),
            branch_found, *next_pc);

    statsFTG.ftSizeDist.sample(search_addr - start_addr);

    // Finally set the BPU PC to the next FT in the next cycle
    set(cur_pc, *next_pc);

    // ftq->printFTQ(tid);
}



/// Post fetch part ------------------------------------------


bool
FTG::updatePreDecode(ThreadID tid, const InstSeqNum seqNum,
                     const StaticInstPtr &inst, PCStateBase &pc,
                     const FetchTargetPtr &ft)
{
    assert(ft != nullptr);
    // The PC must be in the range of the fetch target.
    assert(ft->inRange(pc.instAddr()));

    assert(ft->getFetchSeqNum() == ftq->readHead(tid)->getFetchSeqNum());
    BranchType brType = branch_prediction::getBranchType(inst);
    statsFTG.preDecUpdate[brType]++;

    DPRINTF(FTG, "%s(tid:%i, sn:%lu, inst: %s, PC:%#x, FT[%llu, taken:%i, "
            "end:#%#x)\n", __func__, tid, seqNum,
            branch_prediction::toString(brType), pc.instAddr(), ft->getFetchSeqNum(),
            ft->isTaken(), ft->endAddress());

    bool target_set = false;
    BPredUnit::PredictorHistory* hist = nullptr;

    // The fetch stage will call this function after pre-decoding an
    // instruction finds a branch instruction. Check if this is the exit
    // branch.
    if (ft->isEndBranch(pc.instAddr())
        && ft->bpu_history != nullptr) {

        // Pop the history from the FTQ to move it later to the
        // history buffer.
        hist = static_cast<BPredUnit::PredictorHistory*>(ft->bpu_history);
        ft->bpu_history = nullptr;

        DPRINTF(FTG, "Pop history from FT:%llu => sn:%llu, PC:%#x, taken:%i, "
                "target:%#x\n", ft->getFetchSeqNum(), seqNum, hist->pc,
                hist->predTaken, hist->target->instAddr());

    }

    // Check if we have a valid history. If not we need to create one.
    if (hist == nullptr) {
        DPRINTF(FTG, "[tid:%i, sn:%llu] No branch history for PC:%#x\n",
                tid, seqNum, pc.instAddr());
        statsFTG.noHistType[brType]++;

        // The branch was not detected by the FTG stage in the first place
        // because the BTB did not had an entry for this PC. It can happen
        // if this is the first time the branch is encountered, the branch
        // was never taken before, or the entry got evicted.
        //
        // Create a "dummy" history object by assuming the branch is not
        // taken. This will allow the BPU to fix its histories and internal
        // state in case the assumption was wrong. It works because for
        // FDP we use "taken" history where not taken branches don't modify
        // the global history.

        hist = new BPredUnit::PredictorHistory(tid, seqNum,
                                               pc.instAddr(), inst);
        bpu->branchPlaceholder(tid, pc.instAddr(), inst->isUncondCtrl(),
                               hist->bpHistory);

        hist->predTaken = hist->condPred = false;
        hist->targetProvider = BPredUnit::TargetProvider::NoTarget;

        set(hist->target, std::unique_ptr<PCStateBase>(pc.clone()));
        inst->advancePC(*hist->target);

    }

    assert(hist != nullptr);
    assert(hist->type == brType);

    // Assign the branch instruction instance its sequence number
    // and push the history to the main history buffer.
    hist->seqNum = seqNum;
    bpu->predHist[tid].push_front(hist);

    // Finally update the current fetch PC if not already done.
    // For taken branches the target is stored in the FTQ. For not taken
    // branches we need to advance the PC.
    if (!target_set) {
        if (hist->predTaken) {
            set(pc, ft->getPredictedTarget());
        } else {
            inst->advancePC(pc);
        }
    }

    DPRINTF(FTG, "%s done. next PC:%s\n", __func__, pc);
    return hist->predTaken;
}


bool
FTG::updatePC(const DynInstPtr &inst,
              PCStateBase &fetch_pc, FetchTargetPtr &ft)
{
    // This function will update the fetch PC to the next instruction.
    // If the current instruction is a branch it will make
    // the branch prediction.
    bool predict_taken;
    ThreadID tid = inst->threadNumber;


    if (inst->isControl()) {
        // The instruction is a control instruction.

        // With a decoupled front-end the branch prediction was done
        // while creating the fetch target. Now update the prediction
        // with the information from the predecoding.
        predict_taken = updatePreDecode(tid, inst->seqNum,
                                        inst->staticInst, fetch_pc, ft);

        DPRINTF(FTG, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted %s to go to %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(),
                predict_taken ? "taken" : "not taken",
                fetch_pc);
        inst->setPredTarg(fetch_pc);
        inst->setPredTaken(predict_taken);

        ++statsFTG.branches;

        if (predict_taken) {
            ++statsFTG.predTakenBranches;
        }

    } else {

        // For non-branch instructions simply advance the PC.
        inst->staticInst->advancePC(fetch_pc);
        inst->setPredTarg(fetch_pc);
        inst->setPredTaken(false);
        predict_taken = false;
    }


    if (ft->isEndInst(inst->pcState().instAddr()) || !ftq->isValid(tid)) {

        DPRINTF(FTG, "[tid:%i][ft:%llu] Reached end of Fetch Target\n",
                        tid, ft->getFetchSeqNum());

        ft = nullptr;
    }

    return predict_taken;
}


void
FTG::drainResume()
{
    DPRINTF(Drain, "Resume from draining.\n");
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].drain = false;
    }
}

void
FTG::drainSanityCheck() const
{
    assert(isDrained());

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(ftgStatus[i] == Idle || stalls[i].drain);
        assert(ftq->isEmpty(i));
    }

    bpu->drainSanityCheck();
}

bool
FTG::isDrained() const
{
    // Make sure the FTQ is empty and the state of all threads is idle.
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify FTQs are drained
        if (!ftq->isEmpty(i))
            return false;

        // Return false if not idle or drain stalled
        if (ftgStatus[i] != Idle) {
            return false;
        }
    }
    return true;
}


void
FTG::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}


bool
FTG::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].fetch) {
        DPRINTF(FTG,"[tid:%i] Fetch stall detected.\n",tid);
        ret_val = true;
    }

    if (stalls[tid].bpu) {
        DPRINTF(FTG,"[tid:%i] BPU stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

void
FTG::profileCycle(ThreadID tid)
{
    switch (ftgStatus[tid]) {
    case Idle:
        statsFTG.idleCycles++;
        break;
    case Running:
        statsFTG.runCycles++;
        break;
    case Squashing:
        statsFTG.squashCycles++;
        break;
    case FTQFull:
        statsFTG.ftqFullCycles++;
        break;

    default:
        break;
    }
}


FTG::FTGStats::FTGStats(o3::CPU *cpu, FTG *ftg)
    : statistics::Group(cpu, "ftg"),

    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
            "Number of cycles FTG is idle. (PC invalid)"),
    ADD_STAT(runCycles, statistics::units::Cycle::get(),
            "Number of cycles FTG is running"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
            "Number of cycles FTG is squashing"),
    ADD_STAT(ftqFullCycles, statistics::units::Cycle::get(),
            "Number of cycles FTG has spent waiting for FTQ to become free"),

    ADD_STAT(fetchTargets, statistics::units::Count::get(),
            "Number of fetch targets created "),
    ADD_STAT(branches, statistics::units::Count::get(),
            "Number of branches that FTG encountered"),
    ADD_STAT(predTakenBranches, statistics::units::Count::get(),
            "Number of branches that FTG predicted taken."),
    ADD_STAT(branchMisspredict, statistics::units::Count::get(),
            "Number of branches that FTG has predicted taken"),
    ADD_STAT(noBranchMisspredict, statistics::units::Count::get(),
            "Number of branches that FTG has predicted taken"),
    ADD_STAT(squashBranchDecode, statistics::units::Count::get(),
            "Number of branches squashed from decode"),
    ADD_STAT(squashBranchCommit, statistics::units::Count::get(),
            "Number of branches squashed from decode"),
    ADD_STAT(preDecUpdate, statistics::units::Count::get(),
            "Number of branches extracted from the predecoder"),
    ADD_STAT(noHistType, statistics::units::Count::get(),
            "Number and type of branches that where undetected by the BPU."),
    ADD_STAT(typeMissmatch, statistics::units::Count::get(),
            "Number branches where the branch type miss match"),
    ADD_STAT(multiBranchInst, statistics::units::Count::get(),
            "Number branches because its not the last branch."),
    ADD_STAT(ftSizeDist, statistics::units::Count::get(),
             "Number of bytes per fetch target")
{
    using namespace statistics;

    ftSizeDist
        .init(/* base value */ 0,
              /* last value */ ftg->fetchTargetWidth,
              /* bucket size */ 4)
        .flags(statistics::pdf);

    preDecUpdate
        .init(enums::Num_BranchType)
        .flags(total | pdf);
    noHistType
        .init(enums::Num_BranchType)
        .flags(total | pdf);


    for (int i = 0; i < enums::Num_BranchType; i++) {
        preDecUpdate.subname(i, enums::BranchTypeStrings[i]);
        noHistType.subname(i, enums::BranchTypeStrings[i]);
    }
}

} // namespace o3
} // namespace gem5
