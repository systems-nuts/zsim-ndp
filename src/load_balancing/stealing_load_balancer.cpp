
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

// TBY TODO: only support intra-rank scheduling, since the number of needs is incorrect
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

void StealingLoadBalancer::generateCommandFromUpper(uint32_t upperCommand) {
    reset();
    std::vector<uint32_t> notIdleVec;
    uint32_t numChild = commModule->childEndId - commModule->childBeginId;
    for (uint32_t i = 0; i < numChild; ++i) {
        if (commModule->childQueueReadyLength[i] >= IDLE_THRESHOLD) {
            notIdleVec.push_back(i);
        }
    }
    if (notIdleVec.size() == 0) {
        return;
    }
    uint32_t perChildSize = std::min<uint32_t>(CHUNK_SIZE, upperCommand / notIdleVec.size());
    for (auto id : notIdleVec) {
        this->commands[id] = perChildSize;
    }
}