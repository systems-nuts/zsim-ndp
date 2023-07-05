#include "filter_cache.h"
#include "zsim.h"
#include "task_support/task.h"
#include "task_support/task_timing_core.h"

using namespace task_support;

TaskTimingCore::TaskTimingCore(FilterCache* _l1i, FilterCache* _l1d, uint32_t domain, 
               g_string& _name, TaskUnit* tu) 
    : TimingCore(_l1i, _l1d, domain, _name), taskUnit(tu) {}

void TaskTimingCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle) - 
                            this->beginCycle; };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");
    coreStat->append(cyclesStat);

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");
    coreStat->append(cCyclesStat);

    waitCycles.init("waitCycles", "Wait cycles");
    coreStat->append(&waitCycles);

    auto z = [this]() { return cRec.getUnhaltedCycles(curCycle) - 
                            this->beginCycle - this->waitCycles.get(); };
    LambdaStat<decltype(z)>* workCyclesStat = new LambdaStat<decltype(z)>(z);
    workCyclesStat->init("workCycles", "Cycles that the core is actually executing tasks");
    coreStat->append(workCyclesStat);

    parentStat->append(coreStat);
}

void TaskTimingCore::forwardToNextPhase(THREADID tid) {
    if (this->curCycle < this->phaseEndCycle) {
        this->waitCycles.inc(this->phaseEndCycle + 1 - this->curCycle);
        this->curCycle = this->phaseEndCycle + 1;
        this->phaseEndCycle += zinfo->phaseLength;
        uint32_t cid = getCid(tid);
        TakeBarrier(tid, cid);
    }
}

void TaskTimingCore::loadAndRecord(Address addr) {
    uint64_t startCycle = curCycle;
    curCycle = l1d->load(addr, curCycle);
    cRec.record(startCycle);
}

void TaskTimingCore::storeAndRecord(Address addr) {
    uint64_t startCycle = curCycle;
    curCycle = l1d->store(addr, curCycle);
    cRec.record(startCycle);
}

void TaskTimingCore::bblAndRecord(Address bblAddr, BblInfo* bblInfo) {
    instrs += bblInfo->instrs;
    curCycle += bblInfo->instrs;

    Address endBblAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endBblAddr; 
            fetchAddr+=(1 << lineBits)) {
        uint64_t startCycle = curCycle;
        curCycle = l1i->load(fetchAddr, curCycle);
        cRec.record(startCycle);
    }
}

InstrFuncPtrs TaskTimingCore::GetFuncPtrs() {
    return {LoadAndRecordFunc, StoreAndRecordFunc, BblAndRecordFunc, 
            BranchFunc, PredLoadAndRecordFunc, PredStoreAndRecordFunc,
            FPTR_ANALYSIS, {0}};
}

void TaskTimingCore::LoadAndRecordFunc(THREADID tid, ADDRINT addr) {
    static_cast<TaskTimingCore*>(cores[tid])->loadAndRecord(addr);
}

void TaskTimingCore::StoreAndRecordFunc(THREADID tid, ADDRINT addr) {
    static_cast<TaskTimingCore*>(cores[tid])->storeAndRecord(addr);
}

void TaskTimingCore::BblAndRecordFunc(THREADID tid, ADDRINT bblAddr, 
                                       BblInfo* bblInfo) {
    TaskTimingCore* core = static_cast<TaskTimingCore*>(cores[tid]);
    core->bblAndRecord(bblAddr, bblInfo);
    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;
        uint32_t cid = getCid(tid);
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
    }
}

void TaskTimingCore::PredLoadAndRecordFunc(THREADID tid, ADDRINT addr, 
                                            BOOL pred) {
    if (pred) static_cast<TaskTimingCore*>(cores[tid])->loadAndRecord(addr);
}

void TaskTimingCore::PredStoreAndRecordFunc(THREADID tid, ADDRINT addr, 
                                             BOOL pred) {
    if (pred) static_cast<TaskTimingCore*>(cores[tid])->loadAndRecord(addr);
}


void TaskTimingCore::fetchTask(task_support::TaskPtr t, uint32_t memId) {
    if (!zinfo->SIM_COMM_EVENT) {
        return;
    }
    uint64_t startCycle = t->readyCycle >= curCycle ? t->readyCycle : curCycle;
    curCycle = l1d->forgeAccess(t->taskId, true, startCycle, memId);   
    cRec.record(startCycle);
}

uint64_t TaskTimingCore::recvCommReq(bool isRead, uint64_t startCycle, uint32_t memId) {
    if (!zinfo->SIM_COMM_EVENT) {
        return startCycle;
    }
    startCycle = startCycle >= curCycle ? startCycle : curCycle;
    uint64_t finishCycle = l1d->forgeAccess(0, isRead, startCycle, memId);
    cRec.record(startCycle, false);
    return finishCycle;
}