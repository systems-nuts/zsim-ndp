
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

uint32_t LoadBalancer::IDLE_THRESHOLD = 0;

LoadBalancer::LoadBalancer(uint32_t _level, uint32_t _commId) 
    : level(_level), commId(_commId) {
    assert(_level > 0);
    this->commModule = (CommModule*)zinfo->commModules[level][commId];
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

StealingLoadBalancer::StealingLoadBalancer(uint32_t _level, uint32_t _commId, 
        Config& config) : LoadBalancer(_level, _commId) {
    this->CHUNK_SIZE = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
}

void StealingLoadBalancer::generateCommand() {
    reset();
    srand((unsigned)time(NULL));
    std::vector<uint32_t> idleVec;
    std::vector<uint32_t> notIdleVec;
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        if (commModule->childQueueLength[i] < IDLE_THRESHOLD) {
            idleVec.push_back(i);
        } else if (commModule->childQueueReadyLength[i] >= IDLE_THRESHOLD) {
            notIdleVec.push_back(i);
        }
    }
    if (notIdleVec.size() == 0) {
        return;
    }
    for (size_t i = 0; i < idleVec.size(); ++i) {
        uint32_t theifId = idleVec[i];
        uint32_t victimPos = rand() % notIdleVec.size();
        uint32_t victimId = notIdleVec[victimPos];
        uint32_t cnt = 0;
        if (CHUNK_SIZE == 0) {
            cnt = (commModule->childQueueLength[victimId] - commands[victimId]) / 2;
        } else {
            assert(CHUNK_SIZE > 0);
            cnt = CHUNK_SIZE;
        }
        this->needs[theifId] = cnt;
        this->commands[victimId] += cnt;
        if (commModule->childQueueReadyLength[victimId] - 
                commands[victimId] < IDLE_THRESHOLD) {
            notIdleVec.erase(notIdleVec.begin() + victimPos);
        }
        if (notIdleVec.size() == 0) {
            break;
        }
    }
    // output();
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

ReserveLoadBalancer::ReserveLoadBalancer(uint32_t _level, uint32_t _commId, 
        Config& config) : LoadBalancer(_level, _commId) {
    this->CHUNK_SIZE = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
}

static bool hotter(const DataHotness& a, const DataHotness& b) {
    return a.cnt > b.cnt;
}

void ReserveLoadBalancer::generateCommand() {
    reset();
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    int totalNeeds = 0;
    for (uint32_t i = 0; i < numChild; ++i) {
        uint64_t curLength = commModule->childQueueLength[i];
        if (curLength >= IDLE_THRESHOLD) { 
            this->needs[i] = 0;
        } else {
            this->needs[i] = std::min<uint32_t>(IDLE_THRESHOLD - curLength, CHUNK_SIZE);
        }
        totalNeeds += this->needs[i];
    }
    assert(totalNeeds != 0);
    std::sort(childDataHotness.begin(), childDataHotness.end(), hotter);
    for (auto& item : childDataHotness) {
        // TBY TODO: this can only be used for level-1 load balancing
        uint32_t idx = item.srcBankId - commModule->childBeginId;
        assert(this->needs[idx] == 0);
        // if (this->needs[idx] > 0) {
        //     continue;
        // }
        // info("addr: %lu cnt: %u", item.addr, item.cnt);
        this->commands[idx] += item.cnt;
        totalNeeds -= item.cnt;
        if (totalNeeds <= 0) {
            break;
        }
    }
    // output();
}

// TBY TODO: we can exclude not hot unit here
// TBY TODO: only support intra-rank scheduling
void ReserveLoadBalancer::updateChildStateForLB() {
    if (this->level != 1) {
        return;
    }
    this->childDataHotness.clear();
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        if (this->commModule->childQueueReadyLength[i-commModule->childBeginId]
                <= IDLE_THRESHOLD) {
            continue;
        }
        ReserveLbPimBridgeTaskUnit* tu = (ReserveLbPimBridgeTaskUnit*)zinfo->taskUnits[i];
        tu->sketch.prepareForAccess();
        tu->sketch.getHotItemInfo(this->childDataHotness, HOT_DATA_NUMBER);
    }
}