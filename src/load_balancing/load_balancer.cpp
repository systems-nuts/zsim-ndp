
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
}

void LoadBalancer::assignLbTarget(const std::vector<DataHotness>& outInfo) {
    uint32_t curChildId = 0;
    for (uint32_t i = 0; i < outInfo.size(); ++i) {
        auto item = outInfo[i];
        while (this->needs[curChildId] == 0) {
            if (++curChildId >= this->needs.size()) {
                break;
            }
        }
        if (curChildId >= this->needs.size()) {
            break;
        }
        // info("Assign lb target: cur childId: %u, needs: %u, addr: %lu, cnt: %u",  
        //     curChildId + commModule->childBeginId, this->needs[curChildId], item.addr, item.cnt);
        uint32_t targetCommId = curChildId + commModule->childBeginId;
        this->assignOneAddr(item.addr, targetCommId);
        zinfo->commModules[level-1][targetCommId]->addToSteal(item.cnt);
        this->needs[curChildId] = this->needs[curChildId] < item.cnt ? 
            0 : this->needs[curChildId] - item.cnt;
    }
}

void LoadBalancer::assignOneAddr(Address addr, uint32_t target) {
    this->commModule->addrRemapTable->setChildRemap(addr, target);
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
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        this->needs[i] = 0;
        this->commands[i] = 0;
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

