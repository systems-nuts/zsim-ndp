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

    // for CpuComm
    virtual uint64_t getMinTimeStamp() { panic("!!"); }
    virtual void beginRun(uint64_t newTs) { panic("!!"); }

};

class TaskUnitManager : public GlobAlloc {
protected:
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
        // info("taskUnit %u report finish, all Finish: %u", tuId, finishUnitNumber);
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
    // for CpuComm
    virtual void endOfPhaseAction() {}
    virtual void beginRun() {}
    virtual uint64_t getAllowedTimeStamp() { panic("!!"); }
    virtual bool reportChangeAllowedTimestamp(uint32_t taskUnitId) { panic("!!"); }
};

}

namespace pimbridge {

using namespace task_support;

struct cmp {
    bool operator()(const TaskPtr& t1, const TaskPtr& t2) const {
        if (t1->readyCycle != t2->readyCycle) {
            return t1->readyCycle > t2->readyCycle;
        } else {
            return t1->taskId > t2->taskId;
        }
    }
};
    
} // namespace task_support