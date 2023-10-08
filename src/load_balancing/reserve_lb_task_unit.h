#pragma once
#include <map>
#include <unordered_map>
#include <queue>
#include <deque>
#include "task_support/task_unit.h"
#include "task_support/pim_bridge_task_unit.h"
#include "load_balancing/reserve_sketch.h"
#include "load_balancer.h"


namespace pimbridge {

using namespace task_support;

class ReserveLbPimBridgeTaskUnitKernel 
    : public PimBridgeTaskUnitKernel {
public:
    MemSketch sketch;
private:
    uint64_t reserveRegionSize;
    std::unordered_map<Address, 
        std::priority_queue<TaskPtr, std::deque<TaskPtr>, cmp>> reserveRegion;
public:
    ReserveLbPimBridgeTaskUnitKernel(uint32_t _tuId, uint32_t numBucket, uint32_t bucketSize);

    void taskEnqueueKernel(TaskPtr t, int available) override;
    TaskPtr taskDequeueKernel() override;
    bool isEmpty() override;
    uint64_t getReadyTaskQueueSize() override;

     void executeLoadBalanceCommand(
        const LbCommand& command, 
        std::vector<DataHotness>& outInfo) override;
        
    uint64_t getTopItemLength() override;
    void prepareState() override;

    void exitReserveState(Address lbPageAddr);
private:
    bool shouldReserve(TaskPtr t);
    TaskPtr reservedTaskDequeue();
    void reservedTaskEnqueue(TaskPtr t);

    friend class ReserveLoadBalancer;
};

}