#pragma once
#include "task_support/task_unit.h"
#include "log.h"

namespace task_support {

class CpuCommTaskUnitKernel : public TaskUnitKernel {
private:
    std::deque<TaskPtr> taskQueue;
public:
    CpuCommTaskUnitKernel(uint32_t _tuId);
    ~CpuCommTaskUnitKernel() {}

    void taskEnqueueKernel(TaskPtr t, int available) override;
    TaskPtr taskDequeueKernel() override;
    bool isEmpty() override;
    uint64_t getTaskQueueSize() override;

};

class CpuCommTaskUnit : public TaskUnit {
public:
    CpuCommTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum);

    void assignNewTask(TaskPtr t, Hint* hint) override;
};

}