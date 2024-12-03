#pragma once
#include "comm_support/comm_module.h"
#include "config.h"
#include "log.h"
#include "stats.h"

using namespace task_support;

namespace pimbridge {

// This is the wrapper for all the global dirty works 

class CommModuleManager {
private:
    std::vector<uint64_t> lastToSteal;
    std::vector<uint64_t> lastReady;
    uint64_t CLEAN_STEAL_INTERVAL;

    uint32_t transferSizePerTask;
    uint32_t executeSpeedPerPhase;
public:
    uint32_t STEALER_THRESHOLD, CHUNK_SIZE;
    Counter numSchedTasks, schedTransferSize;

    CommModuleManager(Config& config);
    void clearStaleToSteal(); 
    void returnReplacedAddr(Address lbPageAddr, uint32_t replaceLevel,
                            uint32_t replaceCommId);
    void setDynamicLbConfig();

    uint32_t getTransferSizePerTask() { return transferSizePerTask; }
    uint32_t getExecuteSpeedPerPhase() {return executeSpeedPerPhase; }
private:
    void computeExecuteSpeed();
    void computeTransferRatio();
    void returnReplacedAddrFromLevel(Address lbPageAddr, uint32_t replaceLevel,
                                     uint32_t replaceCommId);
};


}
