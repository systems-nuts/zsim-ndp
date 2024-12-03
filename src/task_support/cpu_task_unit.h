#pragma once
#include "task_support/task_unit.h"
#include "log.h"

namespace task_support {

class CpuTaskUnitKernel : public TaskUnitKernel {
public:
    // static std::deque<TaskPtr> taskQueue;
    std::deque<TaskPtr> taskQueue;

    CpuTaskUnitKernel(uint32_t _tuId, uint32_t _kernelId);
    ~CpuTaskUnitKernel() {}

    void taskEnqueueKernel(TaskPtr t, int available) override;
    TaskPtr taskDequeueKernel() override;
    bool isEmpty() override;
    uint64_t getReadyTaskQueueSize() override;
    uint64_t getAllTaskQueueSize() override;

};

class CpuTaskUnit : public TaskUnit {
public:
    CpuTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum);

    void assignNewTask(TaskPtr t, Hint* hint) override;
};

}