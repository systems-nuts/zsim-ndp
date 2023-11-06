
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

MultiVictimStealingLoadBalancer::MultiVictimStealingLoadBalancer (Config& config, uint32_t _level, 
        uint32_t _commId) : StealingLoadBalancer(config, _level, _commId) {
    this->victimNumber = config.get<uint32_t>("sys.pimBridge.loadBalancer.victimNumber");
}

void MultiVictimStealingLoadBalancer::generateCommand(bool* needParentLevelLb) {
    reset();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        bool isStealer = genDemand(i);
        bool isVictim = genSupply(i);
        assert(!(isStealer && isVictim));
    }
    if (demandIdxVec.empty() || supplyIdxVec.empty()) {
        if (!demandIdxVec.empty() && supplyIdxVec.empty()) {
            *needParentLevelLb = true;
        }
        return;
    }
    DEBUG_LB_O("comm %s command lb", this->commModule->getName());
    outputDemandSupply();
    for (size_t i = 0; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i]; 
        for (uint32_t j = 0; j < victimNumber; ++j) {
            // choose victim & amount
            uint32_t victimPos = rand() % supplyIdxVec.size();
            uint32_t victimIdx = supplyIdxVec[victimPos];
            uint32_t amount = std::min(demand[demandIdxVec[i]]/victimNumber, supply[victimIdx]);
            // update command & assignTable
            this->commands[victimIdx].add(amount);
            this->assignTable[victimIdx].push_back(std::make_pair(stealerIdx, amount));
            // update supply
            supply[victimIdx] -= amount;
            if (supply[victimIdx] == 0) {
                supplyIdxVec.erase(supplyIdxVec.begin() + victimPos);
            }
            if (supplyIdxVec.size() == 0) {
                break;
            }
        }
        if (supplyIdxVec.size() == 0) {
            break;
        }
    }
    outputCommand();
}