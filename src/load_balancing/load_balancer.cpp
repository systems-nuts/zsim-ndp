
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

    // TBY TODO: set IDLE_THRESHOLD dynamically according to speed
    uint32_t oneIdleThreshold = config.get<uint32_t>("sys.pimBridge.loadBalancer.idleThreshold");
    this->IDLE_THRESHOLD = oneIdleThreshold * 
        zinfo->commModules[level-1][commModule->childBeginId]->getNumBanks();

    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    this->commands.resize(numBanks);
    this->demand.resize(numBanks);
    this->supply.resize(numBanks);
    this->canDemand.resize(numBanks);
    this->assignTable.resize(numBanks);
    this->reset();
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
#ifdef DEBUG_LB
    info("---begin lb output---");
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        info("bank: %u, demand: %u, commands: %u", 
            i+commModule->childBeginId, demand[i], commands[i]);
        assert(this->demand[i] == 0 || this->commands[i] == 0);
    }
    info("---end lb output---");
#endif
}

void LoadBalancer::reset() {
    this->demandIdxVec.clear();
    this->supplyIdxVec.clear();
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    if(this->level == 1) {
        this->canDemand.assign(numBanks, true);
    } else {
        this->canDemand.assign(numBanks, false);
    }
    this->demand.assign(numBanks, 0);
    this->supply.assign(numBanks, 0);
    for (auto& c : commands) {
        c.reset();
    }
    for (auto& v : assignTable) {
        v.clear();
    }
}


/*
void LoadBalancer::assignLbTarget(const std::vector<DataHotness>& outInfo) {
    uint32_t curBankId = 0;
    this->generateBankLevelDemand();
    uint32_t lastInNeed = (uint32_t)-1; 

    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    std::vector<uint32_t> assignment(numBanks, 0);

    for (uint32_t i = 0; i < outInfo.size(); ++i) {
        auto item = outInfo[i];
        while (this->bankLevelDemand[curBankId] == 0) {
            ++curBankId;
            if (curBankId >= bankLevelDemand.size()) {
                info("----------------- need lastInNeed! ---------------------");
                curBankId = lastInNeed; 
                break;
            }
        }
        // assert(curBankId < this->bankLevelDemand.size());
        // if (curBankId >= this->demand.size()) {
        //     break;
        // }
        uint32_t targetBankId = curBankId + commModule->bankBeginId;
        // info("Assign lb target: from bank: %u, to bank: %u, demand: %u, addr: %lu, cnt: %u",  
        //     item.srcBankId, targetBankId, this->bankLevelDemand[curBankId], item.addr, item.cnt);
        this->assignOneAddr(item.addr, targetBankId);
        zinfo->commModules[0][targetBankId]->addToSteal(item.cnt);
        assignment[curBankId] += item.cnt;
        this->bankLevelDemand[curBankId] = this->bankLevelDemand[curBankId] < item.cnt ? 
            0 : this->bankLevelDemand[curBankId] - item.cnt;
        lastInNeed = curBankId;
    }
    // if (this->commId == 1) {
    //     for (uint32_t i = 0; i < numBanks; ++i) {
    //         info("assign: i: %u, amount: %u", i, assignment[i]);
    //     }
    // }
}
*/

