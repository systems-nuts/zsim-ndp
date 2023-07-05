#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "config.h"
#include "memory_hierarchy.h"

namespace pimbridge {

// typedef std::pair<Address, uint32_t> DataHotness;

class DataHotness {
public:
    Address addr;
    uint32_t srcBankId;
    uint32_t cnt;
    DataHotness(Address _addr, uint32_t _srcBankId, uint32_t _cnt) : 
        addr(_addr), srcBankId(_srcBankId), cnt(_cnt){}
    void reset() {
        addr = 0;
        cnt = 0;
    }
};

class CommModule;

// The load balancer give commands for children to execute. 
// The commands are integers, indicating the number of tasks that should be scheduled out.
class LoadBalancer {
public:
    static uint32_t IDLE_THRESHOLD;
protected:
    uint32_t level;
    uint32_t commId;
    CommModule* commModule;
    std::vector<uint32_t> commands;
    // std::unordered_map<Address, uint32_t> assignTable;
    std::vector<uint32_t> needs;
public: 
    LoadBalancer(uint32_t _level, uint32_t _commId);
    virtual void generateCommand() = 0;
    virtual void assignLbTarget(const std::vector<DataHotness>& outInfo);
    virtual void updateChildStateForLB() { return; }
protected:
    void assignOneAddr(Address addr, uint32_t target);
    void output();
    void reset();

    friend class CommModule;
};

// The StealingLoadBalancer schedule tasks from the tail of the task queue
// The commands generation is the same to behaviors of work stealing
class StealingLoadBalancer : public LoadBalancer {
private:
    uint32_t CHUNK_SIZE;
public:
    StealingLoadBalancer(uint32_t _level, uint32_t _commId, Config& config);
    void generateCommand() override;
};

// The AverageLoadBalancer schedule tasks from the tail of the task queue
// Try to schedule the number of tasks of each queue to average
class AverageLoadBalancer : public LoadBalancer {
public:
    AverageLoadBalancer(uint32_t _level, uint32_t _commId)
        : LoadBalancer(_level, _commId) {}
    void generateCommand() override;
};

class ReserveLoadBalancer : public LoadBalancer {
private:
    uint32_t CHUNK_SIZE;
    uint32_t HOT_DATA_NUMBER = 5;
    std::vector<DataHotness> childDataHotness;
public:
    ReserveLoadBalancer(uint32_t _level, uint32_t _commId, Config& config);
    void generateCommand() override;
    void updateChildStateForLB() override;
};


}