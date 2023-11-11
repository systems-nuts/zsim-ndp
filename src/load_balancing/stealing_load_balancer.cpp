
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

StealingLoadBalancer::StealingLoadBalancer(Config& config, uint32_t _level, 
        uint32_t _commId) : LoadBalancer(config, _level, _commId) {
}

void StealingLoadBalancer::generateCommand(bool* needParentLevelLb) {
    reset();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        bool isStealer = genDemand(i);
        bool isVictim = genSupply(i);
        assert(!(isStealer && isVictim));
    }
    outputDemandSupply();
    if (demandIdxVec.empty() || supplyIdxVec.empty()) {
        if (!demandIdxVec.empty() && supplyIdxVec.empty()) {
            *needParentLevelLb = true;
        }
        return;
    }
    DEBUG_LB_O("comm %s really command lb", this->commModule->getName());
    for (size_t i = 0; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i]; 
        // choose victim & amount
        uint32_t victimPos = rand() % supplyIdxVec.size();
        uint32_t victimIdx = supplyIdxVec[victimPos];
        uint32_t amount = genScheduleAmount(stealerIdx, victimIdx);
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

bool StealingLoadBalancer::genDemand(uint32_t bankIdx) {
    if (this->canDemand[bankIdx] && 
            commModule->bankQueueLength[bankIdx] < STEALER_THRESHOLD) {
        if (this->chunkScheme == ChunkScheme::Dynamic || this->chunkScheme == ChunkScheme::Static) {
            this->demand[bankIdx] = CHUNK_SIZE;
        }
        this->demandIdxVec.push_back(bankIdx);
        return true;
    } else {
        return false;
    }
}

bool StealingLoadBalancer::genSupply(uint32_t bankIdx) {
    // TBY TODO: generate supply according to speed.
    if (commModule->bankQueueReadyLength[bankIdx] <= VICTIM_THRESHOLD) {
        return false;
    } else {
        this->supply[bankIdx] =  commModule->bankQueueReadyLength[bankIdx] - VICTIM_THRESHOLD;
        this->supplyIdxVec.push_back(bankIdx);
        return true;
    }
    // CommModuleBase* child = zinfo->commModules[this->level-1][i + commModule->childBeginId];
    // uint64_t transferSize = commModule->childTransferSize[i];
    // uint64_t queueLength = commModule->childQueueReadyLength[i];
    // uint64_t transferTime = transferSize / child->getTransferSpeed();
    // uint64_t executeTime = queueLength / child->getExecuteSpeed();
    // if (executeTime > transferTime) {
    //     uint32_t val = uint32_t((executeTime-transferTime) * child->getExecuteSpeed());
    //     this->supplyVec.push_back(std::make_pair(i, val));
    // }
}

uint32_t StealingLoadBalancer::genScheduleAmount(uint32_t stealerIdx, uint32_t victimIdx) {
    if (this->chunkScheme == ChunkScheme::Static || this->chunkScheme == ChunkScheme::Dynamic) {
        return std::min(demand[stealerIdx], supply[victimIdx]);
    } else if (this->chunkScheme == ChunkScheme::HalfVictim) {
        this->demand[stealerIdx] = supply[victimIdx] / 2;
        return supply[victimIdx] / 2;
    } else {
        panic("Unsupported scheme for chunk size!");
    }
}

void StealingLoadBalancer::assignLbTarget(const std::vector<DataHotness>& outInfo) {
    if (outInfo.empty()) {return;}
    DEBUG_LB_O("comm %s assign target", this->commModule->getName());
    uint32_t lastStealerBankId = ((uint32_t)-1);
    for (uint32_t i = 0; i < outInfo.size(); ++i) {
        auto curGive = outInfo[i];
        uint32_t victimBankIdx = curGive.srcBankId - commModule->bankBeginId;
        std::deque<std::pair<uint32_t, uint32_t>>& stealerQueue = assignTable[victimBankIdx];
        if (!stealerQueue.empty()) {
            auto& curReceive = stealerQueue.front();
            uint32_t stealerBankId = curReceive.first + commModule->bankBeginId;
            this->assignOneAddr(curGive.addr, stealerBankId);
            zinfo->commModules[0][stealerBankId]->addToSteal(curGive.cnt);
            if (curGive.cnt > curReceive.second) {
                stealerQueue.pop_front();
            } else {
                curReceive.second -= curGive.cnt;
            }
            lastStealerBankId = stealerBankId;
        } else {
            assert(lastStealerBankId != ((uint32_t)-1));
            this->assignOneAddr(curGive.addr, lastStealerBankId);
            zinfo->commModules[0][lastStealerBankId]->addToSteal(curGive.cnt);
        }
    }
}