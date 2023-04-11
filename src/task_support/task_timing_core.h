#pragma once
#include "core.h"
#include "timing_core.h"
#include "filter_cache.h"
#include "task_unit.h"

namespace task_support {

class TaskTimingCore : public TimingCore {
private:
    TaskUnit* taskUnit;
    uint64_t beginCycle;
    Counter waitCycles;
public:
    TaskTimingCore(FilterCache* _l1i, FilterCache* _l1d, uint32_t domain, 
                   g_string& _name, TaskUnit* tu);
    void initStats(AggregateStat* parentStat);

    InstrFuncPtrs GetFuncPtrs();

    // task support
    bool supportTaskExecution() override { return true; }
    void setBeginCycle() override { 
        this->beginCycle = cRec.getUnhaltedCycles(curCycle);
    }
    void forwardToNextPhase(THREADID tid) override;
 private:
    inline void loadAndRecord(Address addr);
    inline void storeAndRecord(Address addr);
    inline void bblAndRecord(Address bblAddr, BblInfo* bblInstrs);
    inline void record(uint64_t startCycle);

    static void LoadAndRecordFunc(THREADID tid, ADDRINT addr);
    static void StoreAndRecordFunc(THREADID tid, ADDRINT addr);
    static void BblAndRecordFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
    static void PredLoadAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred);
    static void PredStoreAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred);

    static void BranchFunc(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
};

}