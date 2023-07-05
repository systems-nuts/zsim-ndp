
#include <algorithm>
#include "load_balancing/load_balancer.h"
#include "load_balancing/reserve_lb_task_unit.h"
#include "comm_support/comm_module.h"
#include "numa_map.h"
#include "config.h"
#include "zsim.h"

using namespace pimbridge;

ReserveLoadBalancer::ReserveLoadBalancer(Config& config, uint32_t _level, 
        uint32_t _commId) : LoadBalancer(config, _level, _commId) {
    uint32_t oneChunkSize = config.get<uint32_t>("sys.pimBridge.loadBalancer.chunkSize");
    this->CHUNK_SIZE = oneChunkSize * 
        zinfo->commModules[level-1][commModule->childBeginId]->getNumBanks();
    info("LoadBalancer %u-%u, idleThreshold: %u, chunkSize: %u", 
        level, commId, IDLE_THRESHOLD, CHUNK_SIZE);
}

static bool hotter(const DataHotness& a, const DataHotness& b) {
    return a.cnt > b.cnt;
}

// TBY TODO: only support intra-rank scheduling, since the number of needs is incorrect
//           and we use item.srcBankId
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

void ReserveLoadBalancer::generateCommandFromUpper(uint32_t upperCommand) {
    reset();
    std::sort(childDataHotness.begin(), childDataHotness.end(), hotter);
    int totalNeeds = upperCommand;
    for (auto& item : childDataHotness) {
        uint32_t idx = item.srcBankId - commModule->childBeginId;
        this->commands[idx] += item.cnt;
        totalNeeds -= item.cnt;
        if (totalNeeds <= 0) {
            break;
        }
    }
}

// Only support intra-rank scheduling
// But this is all we need. 
// Every load balance command is passed down till the intra-rank level (level1 -> level0)
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