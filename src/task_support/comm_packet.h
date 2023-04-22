#pragma once
#include <cstdint>
#include "task_support/task.h"

namespace task_support {

class CommPacket {
public:
    uint32_t level;
    uint32_t from;
    uint32_t to;
    TaskPtr task;
    CommPacket(uint32_t _level, uint32_t _from, uint32_t _to, TaskPtr _task)
        : level(_level), from(_from), to(_to), task(_task) {
        if (from == 32767) {
            panic("fuck!");
        }
    }
    
    uint32_t getSize() {
        return this->task->taskSize;
    }
};

}