#pragma once
#include <cstdint>
#include "task_support/task.h"

namespace pimbridge {

class CommPacket {
public:
    uint32_t level;
    uint32_t from;
    uint32_t to;
    task_support::TaskPtr task;
    uint64_t readyCycle;
    CommPacket(uint32_t _level, uint32_t _from, uint32_t _to, task_support::TaskPtr _task)
        : level(_level), from(_from), to(_to), task(_task), readyCycle(0) {}
    
    uint32_t getSize() {
        return this->task->taskSize;
    }
};

}