
#include <algorithm>
#include "task_support/task_unit.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "numa_map.h"
#include "log.h"
#include "core.h"
#include "zsim.h"

using namespace task_support;
using namespace pimbridge;


ReserveLbPimBridgeTaskUnitKernel::ReserveLbPimBridgeTaskUnitKernel (
        uint32_t _tuId, uint32_t _kernelId, uint32_t numBucket, uint32_t bucketSize) 
        : PimBridgeTaskUnitKernel(_tuId, _kernelId), 
        sketch(MemSketch(_tuId, numBucket, bucketSize)), 
        reserveRegionSize(0) {
        this->sketch.kernel = this;
    }

void ReserveLbPimBridgeTaskUnitKernel::taskEnqueueKernel(TaskPtr t, int available) {
    assert(available != -1);
    if (available == -2) {
        newNotReadyTask(t);
        return;
    } 
    // maintain reserve information
    Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    this->sketch.enter(lbPageAddr);
    if (shouldReserve(t)) {
        // DEBUG_SKETCH_O("task %lu should reserve, addr: %lu", t->taskId, lbPageAddr);
        this->reservedTaskEnqueue(t);
    } else {
        this->taskQueue.push(t);
    }
}

TaskPtr ReserveLbPimBridgeTaskUnitKernel::taskDequeueKernel() {
    TaskPtr ret = nullptr;
    if (!this->taskQueue.empty()) {
        ret = this->taskQueue.top();
        this->taskQueue.pop();
    } else if (!this->reserveRegion.empty()) {
        ret = this->reservedTaskDequeue();
    } else {
        return this->endTask;
    }

    // maintain reserve information
    this->sketch.exit(zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));

    assert(ret->timeStamp == this->curTs);
    int available = commModule->checkAvailable(
            zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));
    // check whether the task can be run locally
    if (available >= 0) {
        return ret;
    } else if (available == -1) {
        // not available, send to reserve region and retry taskDequeue
        uint64_t curCycle = zinfo->cores[taskUnitId]->getCurCycle();
        TaskCommPacket* p = new TaskCommPacket(ret->timeStamp, curCycle, 0, this->taskUnitId, 1, -1, ret);
        this->commModule->handleOutPacket(p);
        return taskDequeueKernel();
    } else if (available == -2) {
        newNotReadyTask(ret);
        return taskDequeueKernel();
    } else {
        panic("invalid avail! %d", available);
    }
}

bool ReserveLbPimBridgeTaskUnitKernel::isEmpty() {
    return this->taskQueue.empty() && this->notReadyLbTasks.empty() 
        && this->reserveRegion.empty();
}

uint64_t ReserveLbPimBridgeTaskUnitKernel::getReadyTaskQueueSize() {
    return this->taskQueue.size() + this->reserveRegionSize;
}

uint64_t ReserveLbPimBridgeTaskUnitKernel::getAllTaskQueueSize() {
    return this->taskQueue.size() + this->reserveRegionSize 
        + this->notReadyTaskNumber + commModule->toStealSize;
}

void ReserveLbPimBridgeTaskUnitKernel::prepareState() {
    this->sketch.prepareForAccess();
}

void ReserveLbPimBridgeTaskUnitKernel::executeLoadBalanceCommand(
        const LbCommand& command,  
        std::vector<DataHotness>& outInfo) {
    uint64_t curCycle = zinfo->cores[taskUnitId]->getCurCycle();
    std::unordered_map<Address, uint32_t> info;
    info.clear();
    bool noHot = false;
    for (auto curCommand : command.get()) {
        while(curCommand > 0 && this->getReadyTaskQueueSize() > 0) {
            DataHotness item = this->sketch.fetchHotItem();
            if (!noHot && item.cnt == 0) {
                info("no hot data!");
                noHot = true;
                // break;
            }
            if (!noHot) {
                assert(reserveRegion.count(item.addr) != 0 && !reserveRegion[item.addr].empty());
                auto& q = reserveRegion[item.addr];
                int available = this->commModule->checkAvailable(item.addr);
                if (available >= 0) {
                    // because of different timestamps
                    info.insert(std::make_pair(item.addr, q.size()));
                }
                while(!q.empty()) {
                    TaskPtr t = q.top();
                    assert(t->timeStamp == this->curTs);
                    q.pop();
                    #ifdef DEBUG_CHECK_CORRECT
                        Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
                        assert(lbPageAddr == item.addr);
                    #endif
                    this->reserveRegionSize--;
                    TaskCommPacket* p = new TaskCommPacket(t->timeStamp, curCycle, 0, this->taskUnitId, 1, -1, t, 2);
                    this->commModule->handleOutPacket(p);
                    this->commModule->s_ScheduleOutTasks.atomicInc(1);
                    if (curCommand > 0) {
                        curCommand--;
                    }
                }
                reserveRegion.erase(item.addr);
            } else {
                assert(!this->taskQueue.empty());
                TaskPtr t = this->taskQueue.top();
                assert(t->timeStamp == this->curTs);
                this->taskQueue.pop();
                Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
                int available = this->commModule->checkAvailable(lbPageAddr);
                if (available == -2) {
                    newNotReadyTask(t);
                } else if (available == -1) {
                    TaskCommPacket* p = new TaskCommPacket(t->timeStamp, curCycle, 0, this->taskUnitId, 1, -1, t, 3);
                    this->commModule->handleOutPacket(p);
                    this->commModule->s_ScheduleOutTasks.atomicInc(1);
                    --curCommand;
                } else if (available >= 0) {
                    TaskCommPacket* p = new TaskCommPacket(t->timeStamp, curCycle, 0, this->taskUnitId, 1, -1, t, 2);
                    this->commModule->handleOutPacket(p);
                    if (info.count(lbPageAddr) == 0) {
                        info.insert(std::make_pair(lbPageAddr, 0));
                    }
                    info[lbPageAddr] += 1;
                    this->commModule->s_ScheduleOutTasks.atomicInc(1);
                    --curCommand;
                } else {
                    panic("invalid available value");
                }
            }
        }
    }
    for (auto it = info.begin(); it != info.end(); ++it) {
        Address addr = it->first;
        DEBUG_LB_O("unit %u execute lb: addr: %lu, cnt: %u", taskUnitId, addr, it->second);
        outInfo.push_back(DataHotness(addr, this->taskUnitId, it->second));
        this->commModule->newAddrLend(addr);
        DataLendCommPacket* p = new DataLendCommPacket(this->curTs, curCycle, 0, this->taskUnitId,
            1, -1, addr, zinfo->lbPageSize);
        if (this->commModule->toLendMap.count(addr) == 0) {
            this->commModule->toLendMap.insert(std::make_pair(addr, p));
        }
        // this->commModule->handleOutPacket(p);
    }
    if (!zinfo->taskUnits[taskUnitId]->getHasBeenVictim()) {
        zinfo->taskUnits[taskUnitId]->setHasBeenVictim(true);
    }
}

bool ReserveLbPimBridgeTaskUnitKernel::shouldReserve(TaskPtr t) {
    Address pageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    return (this->sketch.isHot(pageAddr));
}

TaskPtr ReserveLbPimBridgeTaskUnitKernel::reservedTaskDequeue() {
    assert(!this->reserveRegion.begin()->second.empty());
    TaskPtr ret = this->reserveRegion.begin()->second.top();
    this->reserveRegion.begin()->second.pop();
    this->reserveRegionSize--;
    
    // Address pageAddr = zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr);
    // DEBUG_SKETCH_O("reserved task %lu dequeue, addr: %lu, size: %lu",
    //     ret->taskId, pageAddr, this->reserveRegion[pageAddr].size());

    if (this->reserveRegion.begin()->second.empty()) {
        this->reserveRegion.erase(this->reserveRegion.begin()->first);
    }
    return ret;
}

void ReserveLbPimBridgeTaskUnitKernel::reservedTaskEnqueue(TaskPtr t) {
    Address pageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    if (this->reserveRegion.find(pageAddr) == reserveRegion.end()) {
        reserveRegion.insert(std::make_pair(pageAddr, 
            std::priority_queue<TaskPtr, std::deque<TaskPtr>, cmp>()));
    }
    this->reserveRegion[pageAddr].push(t);
    DEBUG_SKETCH_O("task %lu ts %lu enter reserve region, addr: %lu, size: %lu", 
        t->taskId, t->timeStamp, pageAddr, this->reserveRegion[pageAddr].size());
    this->reserveRegionSize++;
}

void ReserveLbPimBridgeTaskUnitKernel::exitReserveState(Address lbPageAddr) {
    if (this->reserveRegion.find(lbPageAddr) == reserveRegion.end()) {
        return;
    }
    auto& rq = this->reserveRegion[lbPageAddr];
    DEBUG_SKETCH_O("addr %lu exit reserve state, origin size: %lu", lbPageAddr, rq.size());
    while(!rq.empty()) {
        TaskPtr t = rq.top();
        rq.pop();
        this->reserveRegionSize--;
        this->taskQueue.push(t);
    }
    this->reserveRegion.erase(lbPageAddr);
}
