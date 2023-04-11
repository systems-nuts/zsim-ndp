#pragma once
#include <deque>
#include "galloc.h"
#include "g_std/g_vector.h"
#include "locks.h"
#include "task_support/task.h"


namespace task_support
{

class TaskUnitManager;

class TaskUnit : public GlobAlloc {
protected:
    TaskUnitManager* tum;
    lock_t tuLock;
    TaskPtr endTask; // when calling taskDequeue when the unit is empty, endTask will be returned
    bool isFinished;

public:
    const uint32_t taskUnitId; 
    TaskPtr curTask;

    TaskUnit(uint32_t _tuId, TaskUnitManager* _tum)
        : tum(_tum), endTask(nullptr), isFinished(false), taskUnitId(_tuId), 
          curTask(nullptr) {
        futex_init(&tuLock);
    };

    ~TaskUnit() {}

    virtual void taskEnqueue(TaskPtr t) = 0;
    virtual TaskPtr taskDequeue() = 0;
    virtual void taskFinish(TaskPtr t) = 0;
    virtual uint32_t chooseEnqueueLocation(TaskPtr t) = 0;

    void setEndTask(TaskPtr t) {
        this->endTask = t;
    }
    TaskPtr getEndTask() {
        return this->endTask;
    }
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


class SimpleTaskUnit : public TaskUnit {
private:
    std::deque<TaskPtr>* taskQueue;
public:
    SimpleTaskUnit(uint32_t _tuId, TaskUnitManager* _tum)
        : TaskUnit(_tuId, _tum), taskQueue(new std::deque<TaskPtr>()) {}

    ~SimpleTaskUnit() {
        delete taskQueue;
    }

    void taskEnqueue(TaskPtr t) override;
    TaskPtr taskDequeue() override;
    void taskFinish(TaskPtr t) override;
    uint32_t chooseEnqueueLocation(TaskPtr t) override {
        return this->taskUnitId;
    }
};

    
} // namespace task_support