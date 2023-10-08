#include "comm_support/comm_module.h"
#include "comm_support/comm_module_manager.h"
#include "zsim.h"
#include "config.h"

using namespace task_support;
using namespace pimbridge;

CommModuleManager::CommModuleManager(Config& config) {
    uint32_t size = zinfo->commModules[0].size();
    this->lastReady.resize(size);
    this->lastToSteal.resize(size);
    // this->CLEAN_STEAL_INTERVAL = 10;
    this->CLEAN_STEAL_INTERVAL = config.get<uint32_t>("sys.pimBridge.cleanStealInterval", 0);
}

void CommModuleManager::clearStaleToSteal() {
    if (this->CLEAN_STEAL_INTERVAL == 0 || zinfo->numPhases % this->CLEAN_STEAL_INTERVAL != 0) {
        return;
    }
    for (size_t i = 0; i < zinfo->taskUnits.size(); ++i) {
        uint32_t curReady = zinfo->taskUnits[i]->getCurUnit()->getReadyTaskQueueSize();
        uint32_t curToSteal = zinfo->taskUnits[i]->getCurUnit()->getAllTaskQueueSize() - curReady; 
        if (curReady == 0 && lastReady[i] == 0 
            && curToSteal != 0 && curToSteal == lastToSteal[i]) {
            info("Stale toStealSize, clear it!")
            zinfo->commModules[0][i]->clearToSteal();
        }
        lastToSteal[i] = curToSteal;
        lastReady[i] = curReady;
    }
}