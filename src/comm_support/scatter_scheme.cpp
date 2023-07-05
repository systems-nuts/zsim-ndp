#include <cstdint>
#include "zsim.h"
#include "comm_support/scatter_scheme.h"

using namespace pimbridge;

bool AfterGatherScatter::shouldTrigger() {
    return zinfo->numPhases == commModule->getLastGatherPhase();
}

bool IntervalScatter::shouldTrigger() {
    return (zinfo->numPhases % this->interval == 0);
}

bool OnDemandScatter::shouldTrigger() {
    for (size_t i = 0; i < commModule->scatterBuffer.size(); ++i) {
        if (commModule->scatterBuffer[i].getSize() >= this->threshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastScatterPhase() >= 
            this->maxInterval) {
        return true;
    }
    return false;
}