#include <cstdint>
#include "zsim.h"
#include "comm_support/gather_scheme.h"

using namespace pimbridge;

void GatherScheme::setCommModule(CommModule* _commModule) {
    this->commModule = _commModule; 
    this->bandwidth = (commModule->childEndId - commModule->childBeginId) * packetSize;
}

bool IntervalGather::shouldTrigger() {
    return (zinfo->numPhases % this->interval == 0);
}

bool OnDemandGather::shouldTrigger() {
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        // if (zinfo->commModules[commModule->level-1][i]
        //         ->getParentPackets()->getSize() >= this->threshold) {
        //     return true;
        // }
        if (commModule->childTransferSize[i-commModule->childBeginId] >= this->threshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastGatherPhase() >= 
            this->maxInterval) {
        return true;
    }
    return false;
}

bool OnDemandOfAllGather::shouldTrigger() {
    uint64_t allPackets = 0;
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        // allPackets += zinfo->commModules[commModule->level-1][i]
        //     ->getParentPackets()->getSize();
        allPackets += commModule->childTransferSize[i-commModule->childBeginId];
        if (allPackets >= this->threshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastGatherPhase() >= 
            this->maxInterval) {
        return true;
    }
    return false;
}

bool DynamicGather::enoughTransferPacket() {
    uint64_t curTransferSize = 0;
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        uint64_t curSize = commModule->childTransferSize[i-commModule->childBeginId];
        curTransferSize += curSize >= this->packetSize ? packetSize : curSize;
    }
    return curTransferSize >= highBwUtil * bandwidth;
}

bool DynamicGather::isDangerous() {
    uint64_t idleUnit = 0;
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        uint64_t curSize = commModule->childQueueReadyLength[i-commModule->childBeginId];
        // info("id: %lu, curSize: %lu", i, curSize);
        if (curSize <= 2) {
            idleUnit += 1;
        }
    }
    return (idleUnit > 1);
}

bool DynamicGather::isSafe() {
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        uint64_t curSize = commModule->childQueueReadyLength[i-commModule->childBeginId];
        if (curSize <= 5) {
            return false;
        }
    }
    return true;
}

bool DynamicIntervalGather::shouldTrigger() {
    if (isSafe()) {
        return this->enoughTransferPacket();
    }
    return (zinfo->numPhases - commModule->getLastGatherPhase() >= this->interval);
}

bool DynamicOnDemandGather::shouldTrigger() {
    if (this->enoughTransferPacket()) {
        return true;
    }
    uint32_t curThreshold = this->isDangerous() ? lowThreshold : highThreshold;
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        if (commModule->childTransferSize[i-commModule->childBeginId] >= curThreshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastGatherPhase() >= this->maxInterval) {
        return true;
    }
    return false;
}


bool TaskGenerationTrackGather::shouldTrigger() {
    // Triggered by high bandwidth utilization
    uint64_t curTransferSize = 0;
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        // uint64_t curSize = zinfo->commModules[commModule->level-1][i]
        //     ->getParentPackets()->getSize();
        uint64_t curSize = commModule->childTransferSize[i-commModule->childBeginId];
        curTransferSize += curSize >= this->packetSize ? packetSize : curSize;
    }
    assert(curTransferSize <= bandwidth);
    if (curTransferSize == bandwidth) {
        return true;
    }
    // Triggered by low task generation bandwidth
    assert(curTransferSize >= lastTransferSize);
    uint64_t deltaSize = curTransferSize - lastTransferSize;
    if (deltaSize < 0.5 * avgTaskGenBw) {
        return true;
    }
    return false;
}

void TaskGenerationTrackGather::update() {
    // TBY TODO
}

