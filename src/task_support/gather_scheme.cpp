#include <cstdint>
#include "zsim.h"
#include "task_support/gather_scheme.h"

namespace task_support {

bool IntervalGather::shouldTrigger(CommModule* commModule) {
    return (zinfo->numPhases % this->interval == 0);
}

bool OnDemandGather::shouldTrigger(CommModule* commModule) {
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        if (zinfo->commModules[commModule->level-1][i]->getParentPackets()->size() >= this->threshold) {
            return true;
        }
    }
    return false;
}

}