#include "numa_map.h"
#include "stats.h"
#include "process_local_val.h"
#include "zsim.h"
#include "task_support/hint.h"
#include "task_support/task_unit.h"

using namespace task_support;
using namespace pimbridge;

void PimBridgeTaskUnit::taskEnqueue(TaskPtr t, int available) {
    futex_lock(&tuLock);
    if (this->isFinished) {
        this->isFinished = false;
        tum->reportRestart();
    }
    // assert(commModule->checkAvailable(zinfo->numaMap->getLbPageAddress(t->hint->dataPtr)) != -1);
    // info("unit %u enqueue data: %lu", taskUnitId, zinfo->numaMap->getLbPageAddress(t->hint->dataPtr));
    assert(available != -1);
    if (available == -2) {
        newNotReadyTask(t);
        futex_unlock(&tuLock);
        return;
    }
    this->taskQueue.push(t);
    this->s_EnqueueTasks.inc(1);
    futex_unlock(&tuLock);
}

// TBY TODO: we need to set the current cycle to available cycle
TaskPtr PimBridgeTaskUnit::taskDequeue() {
    if (this->isFinished) {
        return this->endTask;
    }
    futex_lock(&tuLock);
    TaskPtr ret = nullptr;
    if (this->taskQueue.empty()) {
        if (this->notReadyLbTasks.empty()) {
            this->isFinished = true;
            this->tum->reportFinish(this->taskUnitId);
        }
        // info("taskUnit %u wait on notReadyTasks", this->taskUnitId);
        futex_unlock(&tuLock);
        return this->endTask;
    } else {
        ret = this->taskQueue.top();
        this->taskQueue.pop();
    }
    int available = commModule->checkAvailable(
            zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));
    if (available >= 0) {
        this->s_DequeueTasks.inc(1);
        futex_unlock(&tuLock);
        return ret;
    } else if (available == -1) {
        TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, 1, -1, ret);
        this->commModule->handleOutPacket(p);
        futex_unlock(&tuLock);
        return taskDequeue();
    } else if (available == -2) {
        // This happens when a unit lends a data and then borrows it back
        newNotReadyTask(ret);
        futex_unlock(&tuLock);
        return taskDequeue();
    } else {
        panic("invalid avail! %d", available);
    }
}

void PimBridgeTaskUnit::taskFinish(TaskPtr t) {
    this->s_FinishTasks.atomicInc(1);
    return;
}

void PimBridgeTaskUnit::executeLoadBalanceCommand(uint32_t command, 
        std::vector<DataHotness>& outInfo) {
    futex_lock(&tuLock);
    std::unordered_map<Address, uint32_t> info;
    while (!this->taskQueue.empty()) {
        TaskPtr t = this->taskQueue.top();
        this->taskQueue.pop();
        Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
        int available = this->commModule->checkAvailable(lbPageAddr);
        if (available != -2) {
            TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, 1, -1, t, 2);
            this->commModule->handleOutPacket(p);
            if (available >= 0) {
                if (info.count(lbPageAddr) == 0) {
                    info.insert(std::make_pair(lbPageAddr, 0));
                }
                info[lbPageAddr] += 1;
                this->commModule->s_ScheduleOutTasks.atomicInc(1);
            } 
            if (--command == 0) {
                break;
            }
        } else {
            newNotReadyTask(t);
        }
    }
    for (auto it = info.begin(); it != info.end(); ++it) {
        outInfo.push_back(DataHotness(it->first, this->taskUnitId, it->second));
        DataLendCommPacket* p = new DataLendCommPacket(0, this->taskUnitId, 1, -1, it->first, zinfo->lbPageSize);
        assert(this->commModule->checkAvailable(it->first) != -2);
        this->commModule->handleOutPacket(p);
    }
    futex_unlock(&tuLock);
}

void PimBridgeTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    assert(hint->location == -1);
    assert(hint->dataPtr != 0);
    if (hint->firstRound) {
        uint32_t nodeId = zinfo->numaMap->getNodeOfPage(zinfo->numaMap->getPageAddress(hint->dataPtr));
        zinfo->taskUnits[nodeId]->taskEnqueue(t, 0);
    } else {
        int avail = commModule->checkAvailable(zinfo->numaMap->getLbPageAddress(hint->dataPtr));
        if (avail >= 0) {
            zinfo->taskUnits[taskUnitId]->taskEnqueue(t, 0);
        } else {
            CommPacket* p = new TaskCommPacket(0, this->taskUnitId, 1, -1, t);
            this->commModule->handleOutPacket(p);
        }
        this->commModule->s_GenTasks.atomicInc(1);
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

    parentStat->append(tuStat);
}

void PimBridgeTaskUnit::newAddrBorrow(Address lbPageAddr) {
    if (notReadyLbTasks.count(lbPageAddr) == 0) {
        return;
    }
    assert(commModule->checkAvailable(lbPageAddr) >= 0);
    std::deque<TaskPtr>& dq = notReadyLbTasks[lbPageAddr];
    while(!dq.empty()) {
        TaskPtr t = dq.front();
        dq.pop_front();
        this->taskEnqueue(t, 0);
    }
    notReadyLbTasks.erase(lbPageAddr);
}

void PimBridgeTaskUnit::newNotReadyTask(TaskPtr t) {
    Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    if (this->notReadyLbTasks.find(lbPageAddr) == notReadyLbTasks.end()) {
        notReadyLbTasks.insert(std::make_pair(lbPageAddr, std::deque<TaskPtr>()));
    }
    this->notReadyLbTasks[lbPageAddr].push_back(t);
}

