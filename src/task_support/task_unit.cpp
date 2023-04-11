#include <task_support/task_unit.h>

namespace task_support {

void SimpleTaskUnit::taskEnqueue(TaskPtr t) {
    futex_lock(&tuLock);
    if (this->isFinished) {
        this->isFinished = false;
        tum->reportRestart();
    }
    this->taskQueue->push_back(t);
    futex_unlock(&tuLock);
}

TaskPtr SimpleTaskUnit::taskDequeue() {
    if (this->isFinished) {
        return this->endTask;
    }
    futex_lock(&tuLock);
    if (this->taskQueue->empty()) {
        this->isFinished = true;
        this->tum->reportFinish(this->taskUnitId);
        futex_unlock(&tuLock);
        return this->endTask;
    } else {
        TaskPtr ret = this->taskQueue->front();
        this->taskQueue->pop_front();
        futex_unlock(&tuLock);
        return ret;
    }
}

void SimpleTaskUnit::taskFinish(TaskPtr t) {
    return;
}


}