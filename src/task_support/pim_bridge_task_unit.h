#pragma once
#include "task_support/task_unit.h"
 
namespace pimbridge {

class PimBridgeTaskUnit : public task_support::TaskUnit {
protected:
    BottomCommModule* commModule;
    std::unordered_map<Address, std::deque<TaskPtr>> notReadyLbTasks;
    std::priority_queue<TaskPtr, std::deque<TaskPtr>, cmp> taskQueue;
    
public:
    PimBridgeTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
        : TaskUnit(_name, _tuId, _tum) {}

    void assignNewTask(TaskPtr t, Hint* hint) override;
    void taskEnqueue(TaskPtr t, int available) override;
    TaskPtr taskDequeue() override;
    void taskFinish(TaskPtr t) override;

    virtual void executeLoadBalanceCommand(uint32_t command, 
        std::vector<DataHotness>& outInfo);

    virtual uint64_t getTaskQueueSize() { return this->taskQueue.size(); }
    void setCommModule(BottomCommModule* _commModule) { this->commModule = _commModule; }

    void initStats(AggregateStat* parentStat) override;
protected:
    void newAddrBorrow(Address lbPageAddr);
    void newNotReadyTask(TaskPtr t);

    Counter s_EnqueueTasks, s_DequeueTasks, s_FinishTasks;

    friend class pimbridge::BottomCommModule;
};


}