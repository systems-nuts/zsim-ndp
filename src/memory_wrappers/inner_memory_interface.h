#pragma once

#include "g_std/g_string.h"
#include "ddr_mem.h"
#include "memory_hierarchy.h"
#include "config.h"


enum MemorySimModel {
    Simple,           // each MemReq only associates with one dram access
    BoundWeaveModel,  // each MemReq may arise other critical/non-critical accesses
};

class InnerMemoryInterface : public GlobAlloc { 
private:
    MemorySimModel simModel;

public:
    MemObject* memObj;   
    InnerMemoryInterface() : memObj(nullptr) {}
    void init(Config& config, uint32_t lineSize, uint32_t frequency, 
              uint32_t domain, g_string& name, std::string cfgPrefix);
    
    // tby: we do not override functions of MemObject here, since memObj is public. 

private:
    // should be same as the 3 functions in init.cpp
    MemObject* BuildMemoryController(Config& config, uint32_t lineSize,
        uint32_t frequency, uint32_t domain, g_string &name, 
        const std::string& prefix);

    MemObject* BuildMemChannel(Config& config, uint32_t lineSize, 
        uint32_t frequency, uint32_t domain, g_string &name, 
        const std::string& prefix);

    DDRMemory* BuildDDRMemory(Config& config, uint32_t lineSize, 
        uint32_t frequency, uint32_t domain, g_string &name, 
        const std::string& prefix);

};
