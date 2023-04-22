#pragma once

namespace task_support {

class Hint {
public:
    int location; 
    bool firstRound;
    uint64_t dataPtr;
    // location = -1: choose location using schedule
    Hint(int loc, bool _first, uint64_t _data) 
        : location(loc), firstRound(_first), dataPtr(_data) {}
};

}