#include "numa_map.h"
#include "stats.h"
#include "process_local_val.h"
#include "zsim.h"
#include "task_support/hint.h"
#include "task_support/task_unit.h"

namespace task_support {

void PimBridgeTaskUnit::taskEnqueue(TaskPtr t) {
    futex_lock(&tuLock);
    if (this->isFinished) {
        this->isFinished = false;
        tum->reportRestart();
    }
    this->taskQueue->push_back(t);
    this->s_EnqueueTasks.inc(1);
    futex_unlock(&tuLock);
}

TaskPtr PimBridgeTaskUnit::taskDequeue() {
    if (this->isFinished) {
        return this->endTask;
    }
    futex_lock(&tuLock);
    if (this->taskQueue->empty()) {
        this->isFinished = true;
        this->tum->reportFinish(this->taskUnitId);
        futex_unlock(&tuLock);
        return this->endTask;
    } else {
        TaskPtr ret = this->taskQueue->front();
        this->taskQueue->pop_front();
        this->s_DequeueTasks.inc(1);
        futex_unlock(&tuLock);
        return ret;
    }
}

void PimBridgeTaskUnit::taskFinish(TaskPtr t) {
    this->s_FinishTasks.atomicInc(1);
    return;
}

void PimBridgeTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    int location = hint->location;
    uint32_t uid = (uint32_t)-1;

    if (location >= 0) {
        uid = location;
    } else if (location == -1) {
        if (hint->dataPtr == 0) {
            uid = this->taskUnitId;
        } else {
            uint64_t pLineAddr = getPhysicalLineAddr(hint->dataPtr);
            uid = zinfo->numaMap->getNodeOfLineAddr(pLineAddr);
        }
    } else {
        panic("invalid enqueue location");
    }

    if (hint->firstRound || uid == this->taskUnitId) {
        zinfo->taskUnits[uid]->taskEnqueue(t);
    } else {
        this->commModule->generatePacket(uid, t);
        this->s_GenPackets.atomicInc(1);
    }
}

void PimBridgeTaskUnit::initStats(AggregateStat* parentStat) {
    AggregateStat* tuStat = new AggregateStat();
    tuStat->init(name.c_str(), "Task unit stats");

    s_EnqueueTasks.init("enqueueTasks", "Number of enqueued tasks");
    tuStat->append(&s_EnqueueTasks);
    s_DequeueTasks.init("dequeueTasks", "Number of dequeued tasks");
    tuStat->append(&s_DequeueTasks);
    s_FinishTasks.init("finishTasks", "Number of finish tasks");
    tuStat->append(&s_FinishTasks);
    s_GenPackets.init("genPackets", "Number of generated packets");
    tuStat->append(&s_GenPackets);

    parentStat->append(tuStat);
}



}