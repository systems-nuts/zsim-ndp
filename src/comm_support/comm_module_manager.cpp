#include "comm_support/comm_module.h"
#include "comm_support/comm_module_manager.h"
#include "zsim.h"
#include "config.h"
#include "numa_map.h"
#include "debug_output.h"
#include "task_support/pim_bridge_task_unit.h"

using namespace task_support;
using namespace pimbridge;

CommModuleManager::CommModuleManager(Config& config) {
    uint32_t size = zinfo->commModules[0].size();
    this->lastReady.resize(size);
    this->lastToSteal.resize(size);
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
                && curToSteal != 0 && curToSteal == lastToSteal[i]
                && zinfo->taskUnits[i]->getHasBeenVictim()
                && !zinfo->taskUnits[i]->getHasReceiveLbTask()) {
            info("unit %lu Stale toStealSize, clear it!", i);
            zinfo->commModules[0][i]->clearToSteal();
        }
        zinfo->taskUnits[i]->setHasBeenVictim(false);
        zinfo->taskUnits[i]->setHasReceiveLbTask(false);
        lastToSteal[i] = curToSteal;
        lastReady[i] = curReady;
    }
}

void CommModuleManager::returnReplacedAddr(Address lbPageAddr, uint32_t replaceLevel,
                                           uint32_t replaceCommId) {
    assert(zinfo->commModules[2].size() == 1);
#ifdef DEBUG_CHECK_CORRECT
    assert(replaceLevel >= 0 && replaceLevel <= 2);
#endif
    if (replaceLevel == 2) {
        returnReplacedAddrFromLevel(lbPageAddr, 2, 0);
    } else {
        Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
        uint32_t originBankId = zinfo->numaMap->getNodeOfPage(pageAddr);
        uint32_t originLevel1CommId = zinfo->commMapping->getCommId(1, originBankId);
        uint32_t curLevel1CommId = replaceLevel == 1 ? 
            replaceCommId : zinfo->commMapping->getCommId(1, replaceCommId);
        if (curLevel1CommId == originLevel1CommId) {
            returnReplacedAddrFromLevel(lbPageAddr, 1, curLevel1CommId);
        } else {
            returnReplacedAddrFromLevel(lbPageAddr, 2, 0);
        }
    } 
}

void CommModuleManager::returnReplacedAddrFromLevel(
        Address lbPageAddr, uint32_t replaceLevel, uint32_t replaceCommId) {
    uint32_t originBankId = zinfo->numaMap->getNodeOfPage(
        zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr)
    );
    DEBUG_ADDR_RETURN_O("returnReplacedAddrFromLevel: level: %u, comm: %u, addr: %lu, originBank: %u", 
        replaceLevel, replaceCommId, lbPageAddr, originBankId);
    uint32_t curLevel = 0;
    uint32_t curCommId = originBankId; 
    // process addrLend
    while(true) {
        CommModuleBase* cm = zinfo->commModules[curLevel][curCommId];
        cm->getRemapTable()->setAddrLend(lbPageAddr, false);
        ++curLevel;
        if (curLevel == replaceLevel) {
            break;
        }
        curCommId = zinfo->commMapping->getCommId(curLevel, originBankId);
    }
    // process childRemap
    curLevel = replaceLevel;
    curCommId = replaceCommId; 
    while(true) {
        CommModuleBase* cm = zinfo->commModules[curLevel][curCommId];
        int remap = cm->getRemapTable()->getChildRemap(lbPageAddr);
        cm->getRemapTable()->setChildRemap(lbPageAddr, -1);
        cm->getRemapTable()->eraseAddrBorrowMidState(lbPageAddr);
        if (curLevel == 0) {
            ((BottomCommModule*)cm)->taskUnit->newAddrReturn(lbPageAddr);
            break;
        }
        --curLevel;
        if (remap < 0) {
            // this happens when level == 0;
            info("remap = -1; curLevel: %u, curCommId: %u", curLevel, curCommId);
            break;
        }
        curCommId = (uint32_t)remap;
    }
}