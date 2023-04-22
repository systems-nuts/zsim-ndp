#include <cstdint>
#include "zsim.h"
#include "task_support/scatter_scheme.h"

namespace task_support {

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

}