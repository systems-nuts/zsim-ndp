#include "cpu_comm_task_unit.h"
#include "task_unit.h"
#include "log.h"
#include "numa_map.h"
#include "zsim.h"

using namespace task_support;

void CpuCommTaskUnitManager::endOfPhaseAction() {
    if (this->readyForNewTimeStamp){
        info("Finish timestamp: %lu", this->allowedTimeStamp);
        this->readyForNewTimeStamp = false;
        this->allowedTimeStamp++;
        for (auto tu : taskUnits) {
            tu->beginRun(this->allowedTimeStamp);
        }
    }
}

void CpuCommTaskUnitManager::beginRun() {
    assert(this->allowedTimeStamp == 0);
    this->allowedTimeStamp = 1;
    for (auto tu : taskUnits) {
        tu->beginRun(1);
    }
}

bool CpuCommTaskUnitManager::reportChangeAllowedTimestamp(uint32_t taskUnitId) {
    futex_lock(&tumLock);
    bool changed = this->updateAllowedTimestamp(taskUnitId);
    futex_unlock(&tumLock);
    return changed;
}

bool CpuCommTaskUnitManager::updateAllowedTimestamp(uint32_t taskUnitId) {
    uint64_t originTs = this->allowedTimeStamp;
    uint64_t changedTs = this->taskUnits[taskUnitId]->getMinTimeStamp();
    assert((int)changedTs == -1 || changedTs >= this->allowedTimeStamp);
    if (changedTs != this->allowedTimeStamp) {
        uint64_t val = (uint64_t)1<<63;
        for (size_t i = 0; i < this->taskUnits.size(); ++i) {
            uint64_t curTs = this->taskUnits[i]->getMinTimeStamp();
            // info("i: %u curTs: %d", i, (int)curTs);
            if ((int)curTs == -1) {
                continue;
            }
            val = curTs < val ? curTs : val;
        }
        if (val != originTs) {
            this->readyForNewTimeStamp = true;
            return true;
        } else {
            return false;
        }
    }
    return false; 
}

CpuCommTaskUnit::CpuCommTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
    : TaskUnit(_name, _tuId, _tum) {
    this->curTaskQueue = &(this->taskQueue2);
    this->nxtTaskQueue = &(this->taskQueue1);   
}

void CpuCommTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    assert(hint->location == -1);
    assert(hint->dataPtr != 0);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(zinfo->numaMap->getPageAddress(hint->dataPtr));
    zinfo->taskUnits[nodeId]->taskEnqueue(t, 0);
}

void CpuCommTaskUnit::taskEnqueue(TaskPtr t, int available) {
    taskEnqueueNxtRound(t);
}

TaskPtr CpuCommTaskUnit::taskDequeue() {
    if (this->isFinished) {
        // info("taskUnit %u is finished", taskUnitId);
        return this->endTask;
    }
    futex_lock(&tuLock);
    if (!this->curTaskQueue->empty()) {
        TaskPtr ret = this->curTaskQueue->front();
        this->curTaskQueue->pop_front();
        futex_unlock(&tuLock);
        return ret;
    } else {
        if (!this->nxtTaskQueue->empty()) {
            this->minTimeStamp = tum->getAllowedTimeStamp() + 1;
            // info("taskUnit %u change timeStamp to %lu", taskUnitId, minTimeStamp);
        } else {
            this->isFinished = true;
            tum->reportFinish(this->taskUnitId);
            this->minTimeStamp = -1;
        }
        futex_unlock(&tuLock);
        bool switchRound = tum->reportChangeAllowedTimestamp(taskUnitId);
        return this->endTask;
    }
}

void CpuCommTaskUnit::taskFinish(TaskPtr t) {
    return;
}

void CpuCommTaskUnit::switchQueue() {
    // info("Task Unit %u switching queue, curWorkload: %u, nxtWorkload: %u", taskUnitId, curWorkload, nxtWorkload);
    futex_lock(&tuLock);
    if (useQ1) {
        curTaskQueue = &taskQueue2;
        nxtTaskQueue = &taskQueue1;
        useQ1 = false;
    } else {
        curTaskQueue = &taskQueue1;
        nxtTaskQueue = &taskQueue2;
        useQ1 = true;
    }
    this->s_TransferTimes.atomicInc(1);
    futex_unlock(&tuLock);
}

void CpuCommTaskUnit::beginRun(uint64_t newTs) {
    if (newTs == 1) {
        assert(this->minTimeStamp == 0);
        this->minTimeStamp = newTs;
    } else {
        assert((int)this->minTimeStamp == -1 || this->minTimeStamp == newTs);
    }
    this->switchQueue();
} 

void CpuCommTaskUnit::checkTimeStampChange(uint64_t newTs) {
    if ((int)this->minTimeStamp == -1) {;
        this->minTimeStamp = newTs;
        this->tum->reportChangeAllowedTimestamp(this->taskUnitId);
        // info("task unit %d change stamp to %d because un finish", taskUnitId, this->minTimeStamp);
        if (this->isFinished) {
            this->isFinished = false;
            tum->reportRestart();
        }
    } else if (this->minTimeStamp > newTs) {
        assert_msg(this->minTimeStamp == newTs + 1 && newTs == tum->getAllowedTimeStamp() &&
            this->curTaskQueue->empty(), "unit: %u, queue size: %lu", taskUnitId, this->curTaskQueue->size());
        // info("task unit %d change stamp to %d because schedule", taskUnitId, this->minTimeStamp);
        this->minTimeStamp = newTs;
        tum->reportChangeAllowedTimestamp(this->taskUnitId);
    } 
}

void CpuCommTaskUnit::taskEnqueueNxtRound(TaskPtr t) {
    futex_lock(&tuLock);
    checkTimeStampChange(t->timeStamp);
    this->nxtTaskQueue->push_back(t);
    s_TransferSize.atomicInc(t->taskSize);
    futex_unlock(&tuLock);
}

void CpuCommTaskUnit::taskEnqueueCurRound(TaskPtr t) {
     futex_lock(&tuLock);
    if (this->curTaskQueue->empty()) {
        checkTimeStampChange(t->timeStamp);
    }
    this->curTaskQueue->push_back(t);
    futex_unlock(&tuLock);
}

void CpuCommTaskUnit::initStats(AggregateStat* parentStat) {
    AggregateStat* tuStat = new AggregateStat();
    tuStat->init(name.c_str(), "Task unit stats");

    s_EnqueueTasks.init("enqueueTasks", "Number of enqueued tasks");
    tuStat->append(&s_EnqueueTasks);
    s_DequeueTasks.init("dequeueTasks", "Number of dequeued tasks");
    tuStat->append(&s_DequeueTasks);
    s_FinishTasks.init("finishTasks", "Number of finish tasks");
    tuStat->append(&s_FinishTasks);
    s_TransferTimes.init("transferTimes", "Number of CPU forwarding");
    tuStat->append(&s_TransferTimes);
    s_TransferSize.init("transferSize", "Size of CPU forwarding");
    tuStat->append(&s_TransferSize);

    parentStat->append(tuStat);
}