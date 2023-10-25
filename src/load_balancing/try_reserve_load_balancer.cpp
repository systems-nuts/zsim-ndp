#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

TryReserveLoadBalancer::TryReserveLoadBalancer(Config& config, uint32_t _level, uint32_t _commId)
    : ReserveLoadBalancer(config, _level, _commId) {}

void TryReserveLoadBalancer::generateCommand(){
    reset();
    this->childDataHotness.clear();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        bool isStealer = genDemand(i);
        bool isVictim = genSupply(i);
        assert(!(isStealer && isVictim));
    }
    if (demandIdxVec.empty() || supplyIdxVec.empty()) {
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
    size_t i = 0;
    for (i = 0; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i];
        // choose victim & amount
        while (demand[stealerIdx] > 0) {
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
            // supply[victimIdx] -= amount;
            supply[victimIdx] -= amount;
            // update curDemand
            demand[stealerIdx] = demand[stealerIdx] > amount ? demand[stealerIdx] - amount : 0;
        }
        if (hotnessIdx == childDataHotness.size()) {
            break;
        }
    }
    for (; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i]; 
        uint32_t victimPos = rand() % supplyIdxVec.size();
        uint32_t victimIdx = supplyIdxVec[victimPos];
        uint32_t amount = std::min(demand[demandIdxVec[i]], supply[victimIdx]);
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
    outputCommand();
}