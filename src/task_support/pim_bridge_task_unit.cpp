#include "numa_map.h"
#include "stats.h"
#include "process_local_val.h"
#include "zsim.h"
#include "task_support/hint.h"
#include "task_support/task_unit.h"

using namespace task_support;
using namespace pimbridge;

void PimBridgeTaskUnit::taskEnqueue(TaskPtr t) {
    futex_lock(&tuLock);
    if (this->isFinished) {
        this->isFinished = false;
        tum->reportRestart();
    }
    int available = this->checkAvailable(t);
    if (available == -1) {
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
        futex_unlock(&tuLock);
        return this->endTask;
    } else {
        ret = this->taskQueue.top();
        this->taskQueue.pop();
    }
    int available = checkAvailable(ret);
    if (available == 1) {
        this->s_DequeueTasks.inc(1);
        futex_unlock(&tuLock);
        return ret;
    } else if (available == 0) {
        TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, -1, ret);
        this->commModule->handleOutPacket(p);
        this->s_GenPackets.atomicInc(1);
        futex_unlock(&tuLock);
        return taskDequeue();
    } else {
        // This happens when a unit lends a data and then borrows it back
        newNotReadyTask(ret);
        futex_unlock(&tuLock);
        return taskDequeue();
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
        int available = this->checkAvailable(t);
        if (available != -1) {
            TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, -1, t, 2);
            this->commModule->handleOutPacket(p);
            if (available == 1) {
                if (info.count(lbPageAddr) == 0) {
                    info.insert(std::make_pair(lbPageAddr, 0));
                }
                info[lbPageAddr] += 1;
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
        newAddrLend(it->first);
    }
    futex_unlock(&tuLock);
}

void PimBridgeTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    int location = hint->location;
    int uid = -1;

    if (location >= 0) {
        uid = location;
    } else if (location == -1) {
        if (hint->dataPtr == 0) {
            uid = this->taskUnitId;
        } 
        int avail = checkAvailable(t);
        if (avail != 0) {
            uid = this->taskUnitId;
        } else {
            Address pageAddr = zinfo->numaMap->getPageAddress(t->hint->dataPtr);
            uid = zinfo->numaMap->getNodeOfPage(pageAddr);  
            if ((uint32_t)uid == this->taskUnitId) {
                assert(zinfo->ENABLE_LOAD_BALANCE);
                uid = -1;
            }
        }
    } else {
        panic("invalid enqueue location");
    }
    if (hint->firstRound || (uint32_t)uid == this->taskUnitId) {
        zinfo->taskUnits[uid]->taskEnqueue(t);
    } else {
        CommPacket* p = new TaskCommPacket(0, this->taskUnitId, uid, t);
        this->commModule->handleOutPacket(p);
        this->s_GenPackets.atomicInc(1);
    }
}

int PimBridgeTaskUnit::checkAvailable(Address lbPageAddr) {
    Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(pageAddr);
    if (nodeId == this->taskUnitId && 
            !this->commModule->addrRemapTable->getAddrLend(lbPageAddr)) {
        return 1;
    } else if (this->commModule->addrRemapTable->getAddrBorrow(lbPageAddr)) {
        assert(nodeId != this->taskUnitId);
        return 1;
    } else if (this->commModule->addrRemapTable->getAddrBorrowMidState(lbPageAddr)) {
        return -1;
    } else {
        return 0;
    }
}

int PimBridgeTaskUnit::checkAvailable(TaskPtr t) {
    Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    return checkAvailable(lbPageAddr);
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

    s_ScheduleOutData.init("scheduleOutData", "Number of scheduled out data");
    tuStat->append(&s_ScheduleOutData);
    s_ScheduleInData.init("scheduleInData", "Number of scheduled in data");
    tuStat->append(&s_ScheduleInData);
    s_InAndOutData.init("inAndOutData", "Number of scheduled in then out data");
    tuStat->append(&s_InAndOutData);
    s_OutAndInData.init("outAndInData", "Number of scheduled out then in data");
    tuStat->append(&s_OutAndInData);
    // s_ScheduleOutTasks.init("scheduleOutTasks", "Number of scheduled out tasks");
    // tuStat->append(&s_ScheduleOutTasks);
    // s_ScheduleInTasks.init("scheduleInTasks", "Number of scheduled in tasks");
    // tuStat->append(&s_ScheduleInTasks);

    parentStat->append(tuStat);
}

void PimBridgeTaskUnit::newAddrLend(Address lbPageAddr) {
    AddressRemapTable* addrRemapTable = commModule->getAddressRemapTable();
    assert(!addrRemapTable->getAddrLend(lbPageAddr) && 
        !addrRemapTable->getAddrBorrowMidState(lbPageAddr));
    // info("unit %u lend data: %lu", this->taskUnitId, lbPageAddr);
    if (addrRemapTable->getAddrBorrow(lbPageAddr)) {
        addrRemapTable->setAddrBorrow(lbPageAddr, false);
        this->s_InAndOutData.atomicInc(1);
    } else if (!addrRemapTable->getAddrLend(lbPageAddr)) {
        addrRemapTable->setAddrLend(lbPageAddr, true);
        this->s_ScheduleOutData.atomicInc(1);
    } else {
        panic("%lu has been scheduled out!", lbPageAddr);
    }
    DataLendCommPacket* p = new DataLendCommPacket(0, this->taskUnitId, -1, lbPageAddr, zinfo->lbPageSize);
    this->commModule->handleOutPacket(p);
}

void PimBridgeTaskUnit::newAddrBorrow(Address lbPageAddr) {
    AddressRemapTable* addrRemapTable = commModule->getAddressRemapTable();
    assert(!addrRemapTable->getAddrBorrow(lbPageAddr));
    // info("unit %u receive data: %lu", this->taskUnitId, lbPageAddr);
    if (addrRemapTable->getAddrLend(lbPageAddr)) {
        addrRemapTable->setAddrLend(lbPageAddr, false);
        this->s_OutAndInData.atomicInc(1);
    } else {
        addrRemapTable->setAddrBorrow(lbPageAddr, true);
        this->s_ScheduleInData.atomicInc(1);
    }
    if (notReadyLbTasks.count(lbPageAddr) == 0) {
        return;
    }
    std::deque<TaskPtr>& dq = notReadyLbTasks[lbPageAddr];
    while(!dq.empty()) {
        TaskPtr t = dq.front();
        dq.pop_front();
        assert(checkAvailable(t) == 1);
        this->taskEnqueue(t);
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

