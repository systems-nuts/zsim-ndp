
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
    this->DYNAMIC_THRESHOLD = config.get<bool>("sys.pimBridge.loadBalancer.dynamicThreshold", false);
    this->STEALER_THRESHOLD = config.get<uint32_t>("sys.pimBridge.loadBalancer.stealerThreshold");
    this->VICTIM_THRESHOLD = config.get<uint32_t>("sys.pimBridge.loadBalancer.victimThreshold");

    std::string scheme = config.get<const char*>("sys.pimBridge.loadBalancer.chunkScheme", "Static");
    if (scheme == "Static") {
        this->chunkScheme = ChunkScheme::Static;
        this->CHUNK_SIZE = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
    } else if (scheme == "Dynamic") {
        this->chunkScheme = ChunkScheme::Dynamic;
        this->CHUNK_SIZE = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
    } else if (scheme == "HalfVictim") {
        this->chunkScheme = ChunkScheme::HalfVictim;
        this->CHUNK_SIZE = 0;
    } else {
        panic("Unsupported scheme for chunk size!");
    }

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
    uint32_t originBankId = zinfo->numaMap->getNodeFromLbPageAddress(addr);
    if (zinfo->commModules[0][originBankId]->checkAvailable(addr) >= 0) {
        return;
    }
    for (uint32_t l = this->level; l >= 1; --l) {
        childLevelCommId = zinfo->commMapping->getCommId(l-1, targetBankId);
        zinfo->commModules[l][curCommId]->newAddrRemap(addr, childLevelCommId, false);
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
        if (val != "None") {
            info("bank: %u, commands: %s", i+commModule->bankBeginId, val.c_str());
        }
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
    this->canDemand.assign(numBanks, true);
    this->demand.assign(numBanks, 0);
    this->supply.assign(numBanks, 0);
    this->remainTransfer.assign(numBanks, 2 * zinfo->bankGatherBandwidth);
    for (auto& c : commands) {
        c.reset();
    }
    for (auto& v : assignTable) {
        v.clear();
    }
}

void LoadBalancer::setDynamicLbConfig() {
    if (this->DYNAMIC_THRESHOLD) {
        this->STEALER_THRESHOLD = zinfo->commModuleManager->STEALER_THRESHOLD;
        this->VICTIM_THRESHOLD = 2 * STEALER_THRESHOLD;        
    }
    if (this->chunkScheme == ChunkScheme::Dynamic) {
        this->CHUNK_SIZE = zinfo->commModuleManager->CHUNK_SIZE;
    }
}

