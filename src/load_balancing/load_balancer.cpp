
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

LoadBalancer::LoadBalancer(Config& config, uint32_t _level, uint32_t _commId) 
    : level(_level), commId(_commId) {
    assert(_level > 0);
    this->commModule = (CommModule*)zinfo->commModules[level][commId];
    uint32_t oneIdleThreshold = config.get<uint32_t>("sys.pimBridge.loadBalancer.idleThreshold");
    this->IDLE_THRESHOLD = oneIdleThreshold * 
        zinfo->commModules[level-1][commModule->childBeginId]->getNumBanks();
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    this->commands.resize(numChild);
    this->needs.resize(numChild);
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    this->bankLevelNeeds.resize(numBanks);
}

void LoadBalancer::assignLbTarget(const std::vector<DataHotness>& outInfo) {
    uint32_t curBankId = 0;
    this->generateBankLevelNeeds();
    uint32_t lastInNeed = (uint32_t)-1; 

    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    std::vector<uint32_t> assignment(numBanks, 0);

    for (uint32_t i = 0; i < outInfo.size(); ++i) {
        auto item = outInfo[i];
        while (this->bankLevelNeeds[curBankId] == 0) {
            ++curBankId;
            if (curBankId >= bankLevelNeeds.size()) {
                info("----------------- need lastInNeed! ---------------------");
                curBankId = lastInNeed; 
                break;
            }
        }
        // assert(curBankId < this->bankLevelNeeds.size());
        // if (curBankId >= this->needs.size()) {
        //     break;
        // }
        uint32_t targetBankId = curBankId + commModule->bankBeginId;
        // info("Assign lb target: from bank: %u, to bank: %u, needs: %u, addr: %lu, cnt: %u",  
        //     item.srcBankId, targetBankId, this->bankLevelNeeds[curBankId], item.addr, item.cnt);
        this->assignOneAddr(item.addr, targetBankId);
        zinfo->commModules[0][targetBankId]->addToSteal(item.cnt);
        assignment[curBankId] += item.cnt;
        this->bankLevelNeeds[curBankId] = this->bankLevelNeeds[curBankId] < item.cnt ? 
            0 : this->bankLevelNeeds[curBankId] - item.cnt;
        lastInNeed = curBankId;
    }
    // if (this->commId == 1) {
    //     for (uint32_t i = 0; i < numBanks; ++i) {
    //         info("assign: i: %u, amount: %u", i, assignment[i]);
    //     }
    // }
}

void LoadBalancer::assignOneAddr(Address addr, uint32_t targetBankId) {
    uint32_t curCommId = this->commId;
    uint32_t childLevelCommId = (uint32_t)-1;
    for (uint32_t l = this->level; l >= 1; --l) {
        childLevelCommId = zinfo->commMapping->getCommId(l-1, targetBankId);
        zinfo->commModules[l][curCommId]->newAddrRemap(addr, childLevelCommId);
        curCommId = childLevelCommId;
    }
    zinfo->commModules[0][targetBankId]->newAddrRemap(addr, 0, true);
}

void LoadBalancer::output() {
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        info("i: %u, needs: %u, commands: %u", i, needs[i], commands[i]);
        assert(this->needs[i] == 0 || this->commands[i] == 0);
    }
    info("------");
}

void LoadBalancer::reset() {
    this->needs.assign(this->needs.size(), 0);
    this->commands.assign(this->commands.size(), 0);
    this->bankLevelNeeds.assign(this->bankLevelNeeds.size(), 0);
}

void LoadBalancer::generateBankLevelNeeds() {
    if (this->level == 1) {
        this->bankLevelNeeds.assign(needs.begin(), needs.end());
        return;
    }
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    uint32_t banksPerComm = numBanks / numChild;
    for(uint32_t i = 0; i < numChild; ++i) {
        uint32_t curNeedPerBank = (this->needs[i]+banksPerComm-1) / banksPerComm;
        for (uint32_t j = i * banksPerComm; j < (i+1) * banksPerComm; ++j) {
            this->bankLevelNeeds[j] = curNeedPerBank;
            // info("bank need: %u %u", j, curNeedPerBank);
        }
    }
}

void AverageLoadBalancer::generateCommand() {
    reset();
    uint64_t totalLength = 0;
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        totalLength += commModule->childQueueLength[i];
    }
    uint64_t avgLength = totalLength / numChild;
    uint32_t totalNeeds = 0;
    for (uint32_t i = 0; i < numChild; ++i) {
        uint64_t curLength = commModule->childQueueLength[i];
        if (curLength < avgLength) {
            this->needs[i] = avgLength - curLength;
            totalNeeds += this->needs[i];
        } 
    }
    for (uint32_t i = 0; i < numChild; ++i) {
        uint64_t curLength = commModule->childQueueLength[i];
        if (curLength > avgLength) {
            this->commands[i] = std::min<uint32_t>(
                curLength - std::max<uint32_t>(IDLE_THRESHOLD, avgLength), 
                totalNeeds);
            totalNeeds -= this->commands[i];
            assert(totalNeeds >= 0);
            if (totalNeeds == 0) {
                break;
            }
        }
    }
    output();
}

