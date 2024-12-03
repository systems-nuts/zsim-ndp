#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;


FastArriveLoadBalancer::FastArriveLoadBalancer(Config& config, uint32_t _level, uint32_t _commId)
    : StealingLoadBalancer(config, _level, _commId) {}


bool FastArriveLoadBalancer::genSupply(uint32_t bankIdx) {
    if (commModule->bankQueueReadyLength[bankIdx] <= VICTIM_THRESHOLD) {
        return false;
    } else if (commModule->bankTransferSize[bankIdx] >= zinfo->bankGatherBandwidth){
        return false; 
    } else {
        this->remainTransfer[bankIdx] = zinfo->bankGatherBandwidth - commModule->bankTransferSize[bankIdx];
        this->supply[bankIdx] = std::min(
            this->remainTransfer[bankIdx] / zinfo->commModuleManager->getTransferSizePerTask(),
            (uint32_t)commModule->bankQueueReadyLength[bankIdx] - VICTIM_THRESHOLD
        );
        this->supplyIdxVec.push_back(bankIdx);
        return true;
    }
}