#pragma once
#include "comm_support/comm_module.h"
#include "config.h"

using namespace task_support;

namespace pimbridge {

// This is the wrapper for all the global dirty works 

class CommModuleManager {
private:
    std::vector<uint64_t> lastToSteal;
    std::vector<uint64_t> lastReady;
    uint64_t CLEAN_STEAL_INTERVAL;
public:
    CommModuleManager(Config& config);
    void clearStaleToSteal(); 
    void returnReplacedAddr(Address lbPageAddr, uint32_t replaceLevel,
                            uint32_t replaceCommId);
    void setDynamicLbConfig();
private:
    void returnReplacedAddrFromLevel(Address lbPageAddr, uint32_t replaceLevel,
                                     uint32_t replaceCommId);
};


}
