#pragma once
#include "task_support/task_unit.h"
#include <map>
#include <unordered_map>
#include <queue>
#include <deque>
#include "load_balancer.h"


namespace pimbridge {

using namespace task_support;

class MemSketch {
private:
    uint32_t taskUnitId;
    const uint32_t NUM_BUCKET;
    const uint32_t BUCKET_SIZE;
    std::vector<DataHotness> hot;

    typedef std::pair<uint32_t, DataHotness> IdxAndDataHotness;
    uint32_t topHotStart;
    std::vector<IdxAndDataHotness> topHot;

public:
    MemSketch(uint32_t _taskUnitId, uint32_t numBucket, uint32_t bucketSize);
    void enter(Address addr);
    void exit(Address addr);

    bool isHot(Address addr);

    void prepareForAccess();
    DataHotness fetchHotItem();
    void getHotItemInfo(std::vector<DataHotness>& info, uint32_t cnt);

private:
    uint32_t getIdx(uint32_t bucketId, uint32_t posId) {
        assert(posId < this->BUCKET_SIZE);
        return bucketId * BUCKET_SIZE + posId;
    }
    uint32_t hashForBucket(Address);
};

class ReserveLbPimBridgeTaskUnit : public PimBridgeTaskUnit {
public:
    MemSketch sketch;
private:
    uint64_t reserveRegionSize;
    std::unordered_map<Address, 
        std::priority_queue<TaskPtr, std::deque<TaskPtr>, cmp>> reserveRegion;
public:
    ReserveLbPimBridgeTaskUnit(const std::string& _name, uint32_t _tuId, 
                               TaskUnitManager* _tum, uint32_t numBucket, 
                               uint32_t bucketSize);
    void taskEnqueue(TaskPtr t, int available) override;
    TaskPtr taskDequeue() override;

    void executeLoadBalanceCommand(uint32_t command, 
        std::vector<DataHotness>& outInfo) override;
    uint64_t getTaskQueueSize() override;
    void exitReserveState(Address lbPageAddr);
private:
    bool shouldReserve(TaskPtr t);
    TaskPtr reservedTaskDequeue();
    void reservedTaskEnqueue(TaskPtr t);

    friend class ReserveLoadBalancer;
};


}