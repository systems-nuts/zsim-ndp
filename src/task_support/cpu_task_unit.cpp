#include <algorithm>
#include "task_support/cpu_task_unit.h"
#include "task_support/task_unit.h"
#include "log.h"
#include "numa_map.h"
#include "zsim.h"

using namespace task_support;

// std::deque<TaskPtr> CpuTaskUnitKernel::taskQueue = std::deque<TaskPtr>();

CpuTaskUnitKernel::CpuTaskUnitKernel(uint32_t _tuId, uint32_t _kernelId) 
    : TaskUnitKernel(_tuId, _kernelId) {}

void CpuTaskUnitKernel::taskEnqueueKernel(TaskPtr t, int available) {
    // taskQueue.push_back(t);
    taskQueue.push_back(t);
}

TaskPtr CpuTaskUnitKernel::taskDequeueKernel() {
    if (!taskQueue.empty()) {
        TaskPtr ret = taskQueue.front();
        taskQueue.pop_front();
        return ret;
    } else {
        return this->endTask;
    }
}

bool CpuTaskUnitKernel::isEmpty() {
    return taskQueue.empty();
}

uint64_t CpuTaskUnitKernel::getReadyTaskQueueSize() {
    return taskQueue.size();
}

uint64_t CpuTaskUnitKernel::getAllTaskQueueSize() {
    return taskQueue.size();
}

CpuTaskUnit::CpuTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
    : TaskUnit(_name, _tuId, _tum) {
    this->taskUnit1 = new CpuTaskUnitKernel(_tuId, 1001);
    this->taskUnit2 = new CpuTaskUnitKernel(_tuId, 1002);
    this->useQ1 = false;
    this->curTaskUnit = this->taskUnit2;
    this->nxtTaskUnit = this->taskUnit1;
}

void CpuTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    assert(hint->location == -1);
    assert(hint->dataPtr != 0);
    // uint32_t nodeId = zinfo->numaMap->getNodeOfPage(zinfo->numaMap->getPageAddress(hint->dataPtr));
    uint32_t nodeId = rand() % (zinfo->taskUnits.size());
    zinfo->taskUnits[nodeId]->taskEnqueue(t, 0);
}