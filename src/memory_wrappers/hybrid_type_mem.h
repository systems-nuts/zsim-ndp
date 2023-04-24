#pragma once
#include "memory_hierarchy.h"
#include "g_std/g_vector.h"
#include "g_std/g_string.h"
#include "mem_channel.h"
#include "mem_ctrls.h"
#include "ddr_mem.h"
#include "inner_memory_interface.h"
#include "config.h"


class HybridWrapperMemory : public MemObject {
private:
    uint32_t memId;
    InnerMemoryInterface* innerMemInterface;
    
public:
    static g_vector<uint32_t> types;
    static uint32_t controllers;

    HybridWrapperMemory(Config& config, uint32_t lineSize, uint32_t frequency, 
                     uint32_t domain, g_string& _name, 
                     const std::string& prefix, 
                     uint32_t _memId) : memId(_memId) {
        if (_memId == 0) {
            initHybridTypeInfo(config, prefix);
        } else {
            assert_msg(controllers != 0 && types.size() != 0, 
                "The information should have been initialized");
        }
        info("Build hybrid memory %u", memId);
        std::stringstream ss; 
        ss << prefix << "mem-type-" << types[memId] << ".";
        this->innerMemInterface = new InnerMemoryInterface();
        this->innerMemInterface->init(config, lineSize, frequency, domain, 
            _name, ss.str());
    }

    static void initHybridTypeInfo(Config& config, const std::string& prefix) {
        controllers = config.get<uint32_t>(prefix + "controllers");
        types.resize(controllers);
        g_vector<uint32_t> typeStarts = ParseList<uint32_t>(
            config.get<const char*>(prefix + "typeRanges"));
        assert_msg(typeStarts[0] == 0 && 
            typeStarts[typeStarts.size()-1] < controllers, 
            "invalid type range!");
        uint32_t currentType = 0;
        for (uint32_t i = 0; i < controllers; ++i) {
            if (i == typeStarts[currentType + 1]) {
                currentType++;
            }
            types[i] = currentType;
        }
    }

    // override functions
    uint64_t access(MemReq& req) override {
        return innerMemInterface->memObj->access(req);
    }
    void initStats(AggregateStat* parentStat) override {
        innerMemInterface->memObj->initStats(parentStat);
    }
    const char* getName() override {
        return innerMemInterface->memObj->getName();
    }
    // MemObject* getActualMemObj() override {
    //     return this->innerMemInterface->memObj;
    // }
    // uint64_t access(MemReq& req, bool isCritical, uint32_t data_size) override { 
    //     return innerMemInterface->memObj->access(req, isCritical, data_size);
    // }
};