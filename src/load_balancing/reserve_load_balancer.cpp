
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

ReserveLoadBalancer::ReserveLoadBalancer(Config& config, uint32_t _level, 
    uint32_t _commId) : StealingLoadBalancer(config, _level, _commId) {}

bool ReserveLoadBalancer::genSupply(uint32_t bankIdx) {
    if (commModule->bankQueueReadyLength[bankIdx] <=  VICTIM_THRESHOLD) {
        return false;
    } else {
        this->supply[bankIdx] =  commModule->bankQueueReadyLength[bankIdx] - VICTIM_THRESHOLD;
        this->supplyIdxVec.push_back(bankIdx);
        uint32_t bid = bankIdx + this->commModule->bankBeginId;
        ReserveLbPimBridgeTaskUnitKernel* tu = (ReserveLbPimBridgeTaskUnitKernel*)
        zinfo->taskUnits[bid]->getCurUnit();
        tu->sketch.getHotItemInfo(this->childDataHotness);
        return true;
    }
}

void ReserveLoadBalancer::generateCommand(bool* needParentLevelLb) {
    reset();
    this->childDataHotness.clear();
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
    std::sort(childDataHotness.begin(), childDataHotness.end(), 
        [this](const DataHotness& a, const DataHotness& b){
            if (a.cnt != b.cnt) {
                return a.cnt > b.cnt;
            } else {
                return (commModule->bankQueueReadyLength[a.srcBankId-commModule->bankBeginId]
                    > commModule->bankQueueReadyLength[b.srcBankId-commModule->bankBeginId]);
            }
        });
    size_t hotnessIdx = 0;
    for (size_t i = 0; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i];
        uint32_t curDemand = demand[stealerIdx];
        // choose victim & amount
        while (curDemand > 0) {
            while(hotnessIdx < childDataHotness.size()) {
                auto& hotnessItem = childDataHotness[hotnessIdx];
                uint32_t bankIdx = hotnessItem.srcBankId - commModule->bankBeginId;
                if (supply[bankIdx] < hotnessItem.cnt) {
                    hotnessIdx++;
                } else {
                    break;
                }
            }
            if (hotnessIdx == childDataHotness.size()) {
                break;
            }
            auto& hotnessItem = childDataHotness[hotnessIdx++];
            uint32_t victimIdx = hotnessItem.srcBankId - commModule->bankBeginId;
            uint32_t amount = hotnessItem.cnt;
            // update command & assignTable
            this->commands[victimIdx].add(amount);
            this->assignTable[victimIdx].push_back(std::make_pair(stealerIdx, amount));
            // update supply
            supply[victimIdx] -= amount;
            // update curDemand
            curDemand = curDemand > amount ? curDemand - amount : 0;
        }
        if (hotnessIdx == childDataHotness.size()) {
            break;
        }
    }
    outputCommand();
}