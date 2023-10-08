
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

StealingLoadBalancer::StealingLoadBalancer(Config& config, uint32_t _level, 
        uint32_t _commId) : LoadBalancer(config, _level, _commId) {
    uint32_t oneChunkSize = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
    CommModuleBase* child = zinfo->commModules[level-1][commModule->childBeginId];
    this->CHUNK_SIZE = oneChunkSize * child->getNumBanks();
}

void StealingLoadBalancer::generateCommand() {
    reset();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        bool isStealer = genDemand(i);
        bool isVictim = genSupply(i);
        assert(!(isStealer && isVictim));
    }
    for (size_t i = 0; i < demandIdxVec.size(); ++i) {
        uint32_t stealerIdx = demandIdxVec[i]; 
        // choose victim & amount
        uint32_t victimPos = rand() % supplyIdxVec.size();
        uint32_t victimIdx = supplyIdxVec[victimPos];
        uint32_t amount = std::max(demand[demandIdxVec[i]], supply[victimIdx]);
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
}

bool StealingLoadBalancer::genDemand(uint32_t bankIdx) {
    if (this->canDemand[bankIdx] && 
            commModule->bankQueueLength[bankIdx] < IDLE_THRESHOLD) { 
        this->demand[bankIdx] = CHUNK_SIZE;
        this->demandIdxVec.push_back(bankIdx);
        return true;
    } else {
        return false;
    }
}

bool StealingLoadBalancer::genSupply(uint32_t bankIdx) {
    // TBY TODO: generate supply according to speed.
    if (commModule->bankQueueReadyLength[bankIdx] <  2 * IDLE_THRESHOLD) {
        return false;
    } else {
        this->supply[bankIdx] =  commModule->bankQueueReadyLength[bankIdx] - 2 * IDLE_THRESHOLD;
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

void StealingLoadBalancer::assignLbTarget(const std::vector<DataHotness>& outInfo) {
    for (uint32_t i = 0; i < outInfo.size(); ++i) {
        auto curGive = outInfo[i];
        std::deque<std::pair<uint32_t, uint32_t>>& stealerQueue = assignTable[curGive.srcBankId];
        // TBY NOTICE: this is when lastInNeed happens in the old implementation
        assert(!stealerQueue.empty());
        auto& curReceive = stealerQueue.front();
        uint32_t targetBankId = curReceive.first + commModule->bankBeginId;
        this->assignOneAddr(curGive.addr, targetBankId);
        zinfo->commModules[0][targetBankId]->addToSteal(curGive.cnt);
        if (curGive.cnt > curReceive.second) {
            stealerQueue.pop_front();
        } else {
            curReceive.second -= curGive.cnt;
        }
    }
}