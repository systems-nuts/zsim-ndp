#include "task_support/task_unit.h"
#include "log.h"
#include "numa_map.h"
#include "zsim.h"

using namespace task_support;


void TaskUnitManager::reportFinish(uint32_t tuId) {
    futex_lock(&tumLock);
    this->finishUnitNumber += 1;
    // info("taskUnit %u report finish, all Finish: %u", tuId, finishUnitNumber);
    futex_unlock(&tumLock);
}

void TaskUnitManager::reportRestart() {
    futex_lock(&tumLock);
    this->finishUnitNumber -= 1;
    futex_unlock(&tumLock);
}

bool TaskUnitManager::allFinish() {
    futex_lock(&tumLock);
    bool res = (finishUnitNumber == taskUnits.size());
    futex_unlock(&tumLock);
    return res;
}

void TaskUnitManager::finishTimeStamp() {
    assert(this->readyForNewTimeStamp);
    info("Finish timestamp: %lu", this->allowedTimeStamp);
    this->readyForNewTimeStamp = false;
    this->allowedTimeStamp++;
    for (auto tu : taskUnits) {
        tu->beginNewTimeStamp(this->allowedTimeStamp);
    }
}

void TaskUnitManager::beginRun() {
    assert(this->allowedTimeStamp == 0);
    this->finishTimeStamp();
    // this->allowedTimeStamp = 1;
    // for (auto tu : taskUnits) {
    //     tu->beginRun(1);
    // }
}

void TaskUnitManager::reportChangeAllowedTimestamp(uint32_t taskUnitId) {
    futex_lock(&tumLock);
    // uint64_t originTs = this->allowedTimeStamp;
    // uint64_t changedTs = this->taskUnits[taskUnitId]->getMinTimeStamp();
    // assert((int)changedTs == -1 || changedTs >= this->allowedTimeStamp);

    uint64_t val = (uint64_t)1<<63;
    for (size_t i = 0; i < this->taskUnits.size(); ++i) {
        uint64_t curTs = this->taskUnits[i]->getMinTimeStamp();
        // info("i: %u curTs: %d", i, (int)curTs);
        if ((int)curTs == -1) {
            continue;
        }
        val = curTs < val ? curTs : val;
    }

    if (val == this->allowedTimeStamp + 1) {
        this->readyForNewTimeStamp = true;
    } else if (val == this->allowedTimeStamp) {
        if (this->readyForNewTimeStamp) {
            this->readyForNewTimeStamp = false;
        }
    } else {
        assert(val == (uint64_t)1<<63);
    }
    futex_unlock(&tumLock);
}