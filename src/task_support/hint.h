#pragma once

namespace task_support {

class Hint {
public:
    const int location;
    Hint(int _location) : location(_location) {};
    ~Hint();
};


}