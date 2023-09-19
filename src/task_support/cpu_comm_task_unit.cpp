#include "task_support/cpu_comm_task_unit.h"
#include "task_support/task_unit.h"
#include "log.h"
#include "numa_map.h"
#include "zsim.h"

using namespace task_support;

CpuCommTaskUnitKernel::CpuCommTaskUnitKernel(uint32_t _tuId) 
    : TaskUnitKernel(_tuId) {}

void CpuCommTaskUnitKernel::taskEnqueueKernel(TaskPtr t, int available) {
    this->taskQueue.push_back(t);
}

TaskPtr CpuCommTaskUnitKernel::taskDequeueKernel() {
    if (!this->taskQueue.empty()) {
        TaskPtr ret = this->taskQueue.front();
        this->taskQueue.pop_front();
        return ret;
    } else {
        return this->endTask;
    }
}

bool CpuCommTaskUnitKernel::isEmpty() {
    return this->taskQueue.empty();
}

uint64_t CpuCommTaskUnitKernel::getTaskQueueSize() {
    return this->taskQueue.size();
}

CpuCommTaskUnit::CpuCommTaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
    : TaskUnit(_name, _tuId, _tum) {
    this->taskUnit1 = new CpuCommTaskUnitKernel(_tuId);
    this->taskUnit2 = new CpuCommTaskUnitKernel(_tuId);
    this->curTaskUnit = this->taskUnit2;
    this->nxtTaskUnit = this->taskUnit1; ;   
}

void CpuCommTaskUnit::assignNewTask(TaskPtr t, Hint* hint) {
    assert(hint->location == -1);
    assert(hint->dataPtr != 0);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(zinfo->numaMap->getPageAddress(hint->dataPtr));
    zinfo->taskUnits[nodeId]->taskEnqueue(t, 0);
}