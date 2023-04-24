#pragma once
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "memory_wrappers/inner_memory_interface.h"

namespace pimbridge {

class PimBankMemory : public MemObject {
private:
    g_string name;
    uint32_t lineSize;
    uint64_t transferLineIdStart, memoryLineIdStart, nTranferLine;

    InnerMemoryInterface* mainMem;
    InnerMemoryInterface* hiddenMem;

    lock_t lock;
public:
    PimBankMemory(Config& config, uint32_t _lineSize, uint32_t frequency, 
                   uint32_t domain, g_string _name, std::string cfgPrefix);
    ~PimBankMemory();

    // override functions

    uint64_t access(MemReq& req) override;

    void initStats(AggregateStat* parentStat) override {
        return mainMem->memObj->initStats(parentStat);
    }

    const char* getName() override {
        return this->name.c_str();
    }

private:
    uint64_t transferRegionAccess(MemReq& req);
    uint64_t bypassRequest(MemReq& req);
    bool inTransferRegion(Address lineAddr);
    uint64_t convertTransferRegionAddr(uint64_t addrKey);



};

}