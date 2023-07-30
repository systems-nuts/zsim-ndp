#include "task_unit.h"
#include "log.h"

namespace task_support {

class CpuCommTaskUnitManager : public TaskUnitManager {
private:
    bool readyForNewTimeStamp;
    uint64_t allowedTimeStamp;
public:
    CpuCommTaskUnitManager() 
        : TaskUnitManager(), readyForNewTimeStamp(false), allowedTimeStamp(0) {}
    ~CpuCommTaskUnitManager() {}

    void endOfPhaseAction() override;
    void beginRun() override;
    bool reportChangeAllowedTimestamp(uint32_t taskUnitId) override;
    uint64_t getAllowedTimeStamp() override {
        return allowedTimeStamp;
    }

private:
    bool updateAllowedTimestamp(uint32_t taskUnitId);
};

class CpuCommTaskUnit : public TaskUnit {
private:
    uint64_t minTimeStamp;
    bool useQ1;
    // tby: Do not use taskQueue1 and taskQueue2 directly. Use curTaskQueue & nxtTaskQueue instead.
    std::deque<TaskPtr> taskQueue1;
    std::deque<TaskPtr> taskQueue2;
    std::deque<TaskPtr>* curTaskQueue;
    std::deque<TaskPtr>* nxtTaskQueue;
public:
    CpuCommTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum);
    ~CpuCommTaskUnit() {}

    void assignNewTask(TaskPtr t, Hint* hint) override;
    void taskEnqueue(TaskPtr t, int available) override;
    TaskPtr taskDequeue() override;
    void taskFinish(TaskPtr t) override;

    void beginRun(uint64_t newTs) override;
    uint64_t getMinTimeStamp() override { return this->minTimeStamp; }
    void initStats(AggregateStat* parentStat) override;
private:
    void switchQueue();
    void checkTimeStampChange(uint64_t newTs);
    void taskEnqueueNxtRound(TaskPtr t);
    void taskEnqueueCurRound(TaskPtr t);
private:
    Counter s_EnqueueTasks, s_DequeueTasks, s_FinishTasks;
    Counter s_TransferTimes, s_TransferSize;

};

}