
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
        uint32_t _tuId, uint32_t numBucket, uint32_t bucketSize) 
        : PimBridgeTaskUnitKernel(_tuId), 
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

    int available = commModule->checkAvailable(
            zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));
    // check whether the task can be run locally
    if (available >= 0) {
        return ret;
    } else if (available == -1) {
        // not available, send to reserve region and retry taskDequeue
        uint64_t curCycle = zinfo->cores[taskUnitId]->getCurCycle();
        TaskCommPacket* p = new TaskCommPacket(curCycle, 0, this->taskUnitId, 1, -1, ret);
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

uint64_t ReserveLbPimBridgeTaskUnitKernel::getTopItemLength() {
    std::vector<pimbridge::DataHotness> tmpHotness;
    this->sketch.getHotItemInfo(tmpHotness, 2);
    uint64_t res = 0;
    for (auto& dh : tmpHotness) {
        res += dh.cnt;
    }
    return res;
}

void ReserveLbPimBridgeTaskUnitKernel::prepareState() {
    this->sketch.prepareForAccess();
}

void ReserveLbPimBridgeTaskUnitKernel::executeLoadBalanceCommand(
        const LbCommand& command,  
        std::vector<DataHotness>& outInfo) {
    uint64_t curCycle = zinfo->cores[taskUnitId]->getCurCycle();
    std::unordered_map<Address, uint32_t> info;
    for (auto curCommand : command.get()) {
        bool noHot = false;
        while(true) {
            DataHotness item = this->sketch.fetchHotItem();
            if (item.cnt == 0) {
                info("no hot data!");
                noHot = true;
                break;
            }
            assert(reserveRegion.count(item.addr) != 0 && !reserveRegion[item.addr].empty());
            auto& q = reserveRegion[item.addr];
            int available = this->commModule->checkAvailable(item.addr);
            if (available >= 0) {
                // because of different timestamps
                info.insert(std::make_pair(item.addr, q.size()));
            }
            while(!q.empty()) {
                TaskPtr t = q.top();
                q.pop();
#ifdef DEBUG_CHECK_CORRECTNESS
                Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
                assert(lbPageAddr == item.addr);
#endif
                this->reserveRegionSize--;
                TaskCommPacket* p = new TaskCommPacket(curCycle, 0, this->taskUnitId, 1, -1, t, 2);
                this->commModule->handleOutPacket(p);
                this->commModule->s_ScheduleOutTasks.atomicInc(1);
                if (curCommand > 0) {
                    curCommand--;
                }
            }
            reserveRegion.erase(item.addr);
        }
        if (noHot) { break; }
    }
    for (auto it = info.begin(); it != info.end(); ++it) {
        DEBUG_LB_O("unit %u execute lb: addr: %lu, cnt: %u", taskUnitId, it->first, it->second);
        outInfo.push_back(DataHotness(it->first, this->taskUnitId, it->second));
        this->commModule->newAddrLend(it->first);
        DataLendCommPacket* p = new DataLendCommPacket(curCycle, 0, this->taskUnitId,
            1, -1, it->first, zinfo->lbPageSize);
        this->commModule->handleOutPacket(p);
    }
    /*
    while(true) {
        DataHotness item = this->sketch.fetchHotItem();
        if (item.cnt == 0) {
            info("no hot data!");
            break;
        }
        assert(reserveRegion.count(item.addr) != 0);
        auto& q = reserveRegion[item.addr];
        assert(!q.empty());
        while(true) {
            TaskPtr t = q.top();
            q.pop();
            this->reserveRegionSize--;
            if (command > 0) {
                command--;
            }
            TaskCommPacket* p = new TaskCommPacket(curCycle, 0, this->taskUnitId, 1, -1, t, 2);
            this->commModule->handleOutPacket(p);
            if (q.empty()) {
                reserveRegion.erase(item.addr);
                break;
            }
            this->commModule->s_ScheduleOutTasks.atomicInc(1);
        }
        int available = commModule->checkAvailable(item.addr);
        if (available >= 0) {
            // because of different timestamps
            outInfo.push_back(DataHotness(item.addr, this->taskUnitId, q.size()));
            DEBUG_SCHED_META_O("unit %u sched data out, avail: %d, addr: %lu", 
                taskUnitId, available, item.addr);
            DataLendCommPacket* p = new DataLendCommPacket(curCycle, 0, 
                this->taskUnitId, 1, -1, item.addr, zinfo->lbPageSize);
            this->commModule->handleOutPacket(p);
        }
        if (command == 0) {
            break;
        }
    }*/
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
