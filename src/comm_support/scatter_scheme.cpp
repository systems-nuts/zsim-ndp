#include <cstdint>
#include "zsim.h"
#include "comm_support/scatter_scheme.h"

using namespace pimbridge;

bool AfterGatherScatter::shouldTrigger(CommModule* commModule) {
    return commModule->gatherJustNow;
}

bool IntervalScatter::shouldTrigger(CommModule* CommModule) {
    return (zinfo->numPhases % this->interval == 0);
}

bool OnDemandScatter::shouldTrigger(CommModule* commModule) {
    for (size_t i = 0; i < commModule->scatterBuffer.size(); ++i) {
        if (commModule->scatterBuffer[i].size() >= this->threshold) {
            return true;
        }
    }
    return false;
}