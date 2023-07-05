
#include <algorithm>
#include "task_support/task_unit.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "numa_map.h"
#include "log.h"
#include "zsim.h"

using namespace task_support;
using namespace pimbridge;

MemSketch::MemSketch(uint32_t _taskUnitId, uint32_t numBucket, uint32_t bucketSize) 
    : taskUnitId(_taskUnitId), NUM_BUCKET(numBucket), BUCKET_SIZE(bucketSize) {
    this->hot.reserve(NUM_BUCKET * BUCKET_SIZE);
    for (uint32_t i = 0; i < NUM_BUCKET * BUCKET_SIZE; ++i) {
        this->hot.push_back(DataHotness(0, this->taskUnitId, 0));
    }
}

void MemSketch::enter(Address addr) {
    uint64_t bucketId = hashForBucket(addr);
    bool found = false;
    uint32_t minCnt = (uint32_t)-1;
    uint32_t minCntIdx = (uint32_t)-1;
    for (uint32_t i = 0; i < BUCKET_SIZE; ++i) {
        DataHotness& cur = hot[getIdx(bucketId, i)];
        if (cur.addr == addr) {
            cur.cnt++;
            found = true;
            break;
        } 
        if (cur.cnt < minCnt) {
            minCnt = cur.cnt;
            minCntIdx = i;
        }
    }
    if (!found) {
        uint32_t idx = getIdx(bucketId, minCntIdx);
        if (minCnt > 0) {
            hot[idx].cnt -= 1;
        }
        if (hot[idx].cnt == 0) {
            hot[idx].addr = addr;
            hot[idx].cnt = 1;
        }
    }
}

void MemSketch::exit(Address addr) {
    uint64_t bucketId = hashForBucket(addr);
    for (uint32_t i = 0; i < BUCKET_SIZE; ++i) {
        DataHotness& cur = hot[getIdx(bucketId, i)];
        if (cur.addr == addr) {
            cur.cnt--;
            if (cur.cnt == 0) {
                cur.addr = 0;
            }
            break;
        } 
    }
}

bool MemSketch::isHot(Address addr) {
    assert(addr != 0);
    uint64_t bucketId = hashForBucket(addr);
    for (uint32_t i = 0; i < BUCKET_SIZE; ++i) {
        if (hot[getIdx(bucketId, i)].addr == addr) {
            return true;
        }
    }
    return false;
}

void MemSketch::prepareForAccess() {
    topHot.clear();
    topHotStart = 0;
    for (uint32_t i = 0; i < this->hot.size(); ++i) {
        if (this->hot[i].cnt == 0) {
            continue;
        }
        topHot.push_back(std::make_pair(i, hot[i]));
    }
    std::sort(topHot.begin(), topHot.end(), 
        [](const IdxAndDataHotness& a, const IdxAndDataHotness& b) {
            return a.second.cnt > b.second.cnt;
        });
}

DataHotness MemSketch::fetchHotItem() {
    if (topHotStart == this->topHot.size()) {
        return DataHotness(0, 0, 0);
    }
    IdxAndDataHotness res = topHot[topHotStart++];
    this->hot[res.first].reset();
    return res.second;
}

void MemSketch::getHotItemInfo(std::vector<DataHotness>& info, uint32_t cnt) {
    for (uint32_t i = 0; i < cnt; ++i) {
        if (i >= this->topHot.size()) {
            break;
        }
        IdxAndDataHotness res = topHot[i];
        info.push_back(res.second);
    }
} 

uint32_t MemSketch::hashForBucket(Address pageAddr) {   
    return pageAddr % this->NUM_BUCKET;
}

ReserveLbPimBridgeTaskUnit::ReserveLbPimBridgeTaskUnit(
        const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum, 
        uint32_t numBucket, uint32_t bucketSize) 
    : PimBridgeTaskUnit(_name, _tuId, _tum), 
      sketch(MemSketch(taskUnitId, numBucket, bucketSize)), 
      reserveRegionSize(0) {}

void ReserveLbPimBridgeTaskUnit::taskEnqueue(TaskPtr t) {
    futex_lock(&tuLock);
    // maintain finish information
    if (this->isFinished) {
        this->isFinished = false;
        tum->reportRestart();
    }
    // check available
    int available = this->checkAvailable(t);
    if (available == -1) {
        newNotReadyTask(t);
        futex_unlock(&tuLock);
        return;
    } else if (available == 0) {
        TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, -1, t);
        this->commModule->handleOutPacket(p);
        futex_unlock(&tuLock);
        return;
    }
    // actually enter
    // maintain reserve information
    Address lbPageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    this->sketch.enter(lbPageAddr);
    if (shouldReserve(t)) {
        this->reservedTaskEnqueue(t);
    } else {
        this->taskQueue.push(t);
    }
    this->s_EnqueueTasks.inc(1);
    futex_unlock(&tuLock);
}

TaskPtr ReserveLbPimBridgeTaskUnit::taskDequeue() {
    if (this->isFinished) {
        return this->endTask;
    }
    futex_lock(&tuLock);

    // find a task to dequeue
    TaskPtr ret = nullptr;
    if (!this->taskQueue.empty()) {
        ret = this->taskQueue.top();
        this->taskQueue.pop();
    } else if (!this->reserveRegion.empty()) {
        assert(this->reserveRegionSize != 0);
        ret = this->reservedTaskDequeue();
    } else {
        if (this->notReadyLbTasks.empty()) {
            this->isFinished = true;
            this->tum->reportFinish(this->taskUnitId);
        }
        futex_unlock(&tuLock);
        return this->endTask;
    }

    // maintain reserve information
    this->sketch.exit(zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));

    // check whether the task can be run locally
    int available = checkAvailable(ret);
    if (available == 1) {
        this->s_DequeueTasks.inc(1);
        futex_unlock(&tuLock);
        return ret;
    } else if (available == 0) {
        // not available, send to reserve region and retry taskDequeue
        TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, -1, ret);
        this->commModule->handleOutPacket(p);
        this->s_GenPackets.atomicInc(1);
        futex_unlock(&tuLock);
        return taskDequeue();
    } else {
        newNotReadyTask(ret);
        futex_unlock(&tuLock);
        return taskDequeue();
    }
}

uint64_t ReserveLbPimBridgeTaskUnit::getTaskQueueSize() {
    return this->taskQueue.size() + this->reserveRegionSize;
}

bool ReserveLbPimBridgeTaskUnit::shouldReserve(TaskPtr t) {
    Address pageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    return (this->sketch.isHot(pageAddr));
}

void ReserveLbPimBridgeTaskUnit::executeLoadBalanceCommand(uint32_t command, 
        std::vector<DataHotness>& outInfo) {
    futex_lock(&tuLock);
    while(true) {
        DataHotness item = this->sketch.fetchHotItem();
        if (item.cnt == 0) {
            info("no hot data!");
            break;
        }
        outInfo.push_back(item);
        auto& q = reserveRegion[item.addr];
        assert(!q.empty());
        while(true) {
            TaskPtr t = q.top();
            q.pop();
            this->reserveRegionSize--;
            if (command > 0) {
                command--;
            }
            TaskCommPacket* p = new TaskCommPacket(0, this->taskUnitId, -1, t, 2);
            this->commModule->handleOutPacket(p);
            if (q.empty()) {
                reserveRegion.erase(item.addr);
                break;
            }
        }
        int avail = checkAvailable(item.addr);
        Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(item.addr);
        uint32_t node = zinfo->numaMap->getNodeOfPage(pageAddr);
        bool addrLend = this->commModule->addrRemapTable->getAddrLend(item.addr);
        assert_msg(avail == 1, "data %lu not available by %d, taskUnit: %u, originNode: %u, lend: %d", 
            item.addr, avail, taskUnitId, node, addrLend);
        newAddrLend(item.addr);
        if (command == 0) {
            break;
        }
    }
    futex_unlock(&tuLock);
}

TaskPtr ReserveLbPimBridgeTaskUnit::reservedTaskDequeue() {
    TaskPtr ret = this->reserveRegion.begin()->second.top();
    this->reserveRegion.begin()->second.pop();
    this->reserveRegionSize--;
    if (this->reserveRegion.begin()->second.empty()) {
        this->reserveRegion.erase(this->reserveRegion.begin()->first);
    }
    return ret;
}

void ReserveLbPimBridgeTaskUnit::reservedTaskEnqueue(TaskPtr t) {
    Address pageAddr = zinfo->numaMap->getLbPageAddress(t->hint->dataPtr);
    if (this->reserveRegion.find(pageAddr) == reserveRegion.end()) {
        reserveRegion.insert(std::make_pair(pageAddr, 
            std::priority_queue<TaskPtr, std::deque<TaskPtr>, cmp>()));
    }
    this->reserveRegion[pageAddr].push(t);
    this->reserveRegionSize++;
}