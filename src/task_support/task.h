#pragma once
#include <cstdint>
#include <vector>

#include "task_support/hint.h"

namespace task_support {

class Task;
typedef Task* TaskPtr;

class Task {
public:
    enum TaskState {
        IDLE,
        RUNNING, 
        COMPLETED
    };
    TaskState state;
    const uint64_t taskId; 
    const uintptr_t taskFn;
    uint64_t timeStamp;
    std::vector<uint64_t> args;
    bool isEndTask;

    Hint* hint;
    uint32_t taskSize;
    uint64_t readyCycle;

    Task(uint64_t _taskId, uintptr_t _func, uint64_t _ts, 
         const std::vector<uint64_t>& _args, bool _isEnd, Hint* _hint, 
         uint64_t _readyCycle) 
        : taskId(_taskId), taskFn(_func), timeStamp(_ts), 
          args(_args), isEndTask(_isEnd), hint(_hint), readyCycle(_readyCycle) {
        taskSize = 20 + args.size() * 8;
    }
        
    ~Task() {}

};

}