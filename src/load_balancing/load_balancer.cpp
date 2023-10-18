
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

    // TBY TODO: set STEALER_THRESHOLD dynamically according to speed
    // uint32_t oneIdleThreshold = config.get<uint32_t>("sys.pimBridge.loadBalancer.stealerThreshold");
    this->STEALER_THRESHOLD = config.get<uint32_t>("sys.pimBridge.loadBalancer.stealerThreshold");
    this->VICTIM_THRESHOLD = config.get<uint32_t>("sys.pimBridge.loadBalancer.victimThreshold");

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

void LoadBalancer::outputCommand() {
#ifdef DEBUG_LB
    info("---begin command output---");
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        std::string val = commands[i].output();
        info("bank: %u, commands: %s", i+commModule->bankBeginId, val.c_str());
    }
    info("---end command output---");
#endif
}

void LoadBalancer::outputDemandSupply() {
#ifdef DEBUG_LB
    info("---begin demand-supply output---");
    uint32_t numBanks = commModule->bankEndId - commModule->bankBeginId;
    for (uint32_t i = 0; i < numBanks; ++i) {
        info("bank: %u, demand: %u, supply: %u", 
            i+commModule->bankBeginId, demand[i], supply[i]);
    }
    info("---end demand-supply output---");
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

