#pragma once
#include <deque>
#include <queue>
#include "galloc.h"
#include "g_std/g_vector.h"
#include "locks.h"
#include "stats.h"
#include "task_support/hint.h"
#include "task_support/task.h"
#include "comm_support/comm_packet.h"
#include "comm_support/comm_module.h"

namespace task_support {

class TaskUnitManager;

class TaskUnit : public GlobAlloc {
protected:
    std::string name;
    const uint32_t taskUnitId; 
    TaskUnitManager* tum;
    lock_t tuLock;
    TaskPtr endTask; // when calling taskDequeue when the unit is empty, endTask will be returned
    bool isFinished;

public:
    TaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
        : name(_name), taskUnitId(_tuId), tum(_tum), endTask(nullptr), isFinished(false) {
        futex_init(&tuLock);
    };

    virtual ~TaskUnit() {}

    virtual void assignNewTask(TaskPtr t, Hint* hint) = 0;
    virtual void taskEnqueue(TaskPtr t, int available) = 0;
    virtual TaskPtr taskDequeue() = 0;
    virtual void taskFinish(TaskPtr t) = 0;

    // Getters and setters
    void setEndTask(TaskPtr t) { this->endTask = t; }
    TaskPtr getEndTask() { return this->endTask; }
    const char* getName() { return name.c_str(); }
    uint32_t getTaskUnitId() { return this->taskUnitId; }

    virtual void initStats(AggregateStat* parentStat) {}
};

class TaskUnitManager : public GlobAlloc {
private:
    g_vector<TaskUnit*> taskUnits;
    lock_t tumLock;
    uint32_t finishUnitNumber;

public:
    TaskUnitManager() : finishUnitNumber(0) {
        futex_init(&tumLock);
    }
    ~TaskUnitManager() {}

    void addTaskUnit(TaskUnit* tu) {
        this->taskUnits.push_back(tu);
    }
    void reportFinish(uint32_t tuId) {
        futex_lock(&tumLock);
        this->finishUnitNumber += 1;
        futex_unlock(&tumLock);
    }
    void reportRestart() {
        futex_lock(&tumLock);
        this->finishUnitNumber -= 1;
        futex_unlock(&tumLock);
    }
    bool allFinish() {
        futex_lock(&tumLock);
        bool res = (finishUnitNumber == taskUnits.size());
        futex_unlock(&tumLock);
        return res;
    }
};

}

namespace pimbridge {

using namespace task_support;

struct cmp {
    bool operator()(const TaskPtr& t1, const TaskPtr& t2) const {
        return t1->readyCycle > t2->readyCycle;
    }
};

class PimBridgeTaskUnit : public TaskUnit {
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
    // Counter s_ScheduleOutTasks, s_ScheduleInTasks, s_ScheduleOutData, s_ScheduleInData;
    // Counter s_InAndOutData, s_OutAndInData;

    friend class pimbridge::BottomCommModule;
};
    
} // namespace task_support