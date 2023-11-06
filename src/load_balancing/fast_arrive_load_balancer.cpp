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
    } else if (commModule->bankTransferSize[bankIdx] >= 1024){
        return false; 
    } else {
        this->supply[bankIdx] =  commModule->bankQueueReadyLength[bankIdx] - VICTIM_THRESHOLD;
        this->supplyIdxVec.push_back(bankIdx);
        return true;
    }
}

void FastArriveLoadBalancer::generateCommand(bool* needParentLevelLb) {
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


void FastArriveLoadBalancer::generateCommandOld(bool* needParentLevelLb) {
    reset();
    this->transferLengthQueue = std::priority_queue<TransferLength, std::deque<TransferLength>, cmp>();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        bool isStealer = genDemand(i);
        bool isVictim = StealingLoadBalancer::genSupply(i);
        assert(!(isStealer && isVictim));
        if (isVictim) {
            uint64_t ts = this->commModule->bankTransferSize[i];
            this->transferLengthQueue.push(TransferLength(i, ts));
        }
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
        bool found = false;
        TransferLength tl = TransferLength(0,0);
        while(!this->transferLengthQueue.empty()) {
            TransferLength tl = this->transferLengthQueue.top();
            transferLengthQueue.pop();
            if (this->supply[tl.bankIdx] > 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        uint32_t victimIdx = tl.bankIdx;
        uint32_t amount = std::min(demand[stealerIdx], supply[victimIdx]);
        this->commands[victimIdx].add(amount);
        this->assignTable[victimIdx].push_back(std::make_pair(stealerIdx, amount));
        uint64_t newTransferLength = tl.transferLength 
            + amount / 3 * zinfo->lbPageSize + amount * 8;
        if (supply[victimIdx] != 0) {
            this->transferLengthQueue.push(TransferLength(victimIdx, newTransferLength));
        }
        if (this->transferLengthQueue.size() == 0) {
            break;
        }
    }
    outputCommand();
}