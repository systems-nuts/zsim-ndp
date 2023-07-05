#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include "galloc.h"
#include "stats.h"
#include "locks.h"
#include "task_support/task.h"
#include "comm_support/comm_packet.h"
#include "comm_support/comm_packet_queue.h"
#include "comm_support/gather_scheme.h"
#include "comm_support/scatter_scheme.h"
#include "load_balancing/load_balancer.h"
#include "load_balancing/address_remap.h"

using namespace task_support;

namespace pimbridge {

class CommModuleBase {
protected:
    std::string name;
    uint32_t level;
    uint32_t commId;

    uint32_t bankBeginId; 
    uint32_t bankEndId;
    uint32_t parentId;
    CommPacketQueue parentPackets;
    CommPacketQueue lbParentPackets;

    // same-level direct communication 
    bool enableInterflow;
    uint32_t siblingBeginId;
    uint32_t siblingEndId;
    std::vector<CommPacketQueue> siblingPackets; 

    lock_t commLock;

    AddressRemapTable* addrRemapTable;
    LoadBalancer* loadBalancer;
    uint64_t toStealSize;

    // data collected from task unit during execution
    Counter s_GenTasks, s_FinishTasks;
    Counter s_GenPackets, s_RecvPackets;

public:
    // initialization
    CommModuleBase(uint32_t _level, uint32_t _commId, bool _enableInterflow);
    void initSiblings(uint32_t sibBegin, uint32_t sibEnd);

    virtual uint64_t communicate(uint64_t curCycle) = 0; 
    virtual void gatherState() {}
    virtual bool isEmpty();
    void receivePackets(CommModuleBase* srcModule, 
                        uint32_t messageSize, uint64_t readyCycle, 
                        uint32_t& numPackets, uint32_t& totalSize);
    virtual CommPacket* nextPacket(uint32_t fromLevel, uint32_t fromCommId, 
                                   uint32_t sizeLimit) = 0;
    void handleOutPacket(CommPacket* packet);  

    // load balance
    virtual void commandLoadBalance(uint64_t curCycle) = 0; 
    virtual void executeLoadBalance(uint32_t command, 
        std::vector<DataHotness>& outInfo) = 0;
    void addToSteal(uint32_t num);

    // update through the hierarchy
    // void finishTask();
    // void generateTask();

    // state that accessed by filtered command
    virtual uint64_t stateLocalTaskQueueSize() = 0;
    uint64_t stateTransferRegionSize();
    uint64_t stateToStealSize() { return this->toStealSize; }
    
    // getters and setters
    void setParentId(uint32_t _parentId) { this->parentId = _parentId; }
    const char* getName() { return this->name.c_str(); }
    uint32_t getBankBeginId() { return bankBeginId; }
    uint32_t getBankEndId() { return bankEndId; }
    uint32_t getNumBanks() {return bankEndId - bankBeginId; }
    uint32_t getLevel() { return this->level; }
    uint32_t getCommId() { return this->commId; }
    AddressRemapTable* getAddressRemapTable() { return addrRemapTable; }
    void setLoadBalancer(LoadBalancer* lb) { this->loadBalancer = lb; }

protected:
    virtual bool isChild(int id) = 0;
    virtual void handleInPacket(CommPacket* packet) = 0;
    void interflow(uint32_t sibId, uint32_t messageSize);
    bool isSibling(int id) {
        return (id >= 0 && (uint32_t)id >= siblingBeginId && 
            (uint32_t)id <= siblingEndId && (uint32_t)id != commId);
    }
public:
    virtual void initStats(AggregateStat* parentStat) {}
};

class PimBridgeTaskUnit;

class BottomCommModule : public CommModuleBase{
public:
    PimBridgeTaskUnit* taskUnit;
    BottomCommModule(uint32_t _level, uint32_t _commId, 
                     bool _enableInterflow, PimBridgeTaskUnit* _taskUnit);
    uint64_t communicate(uint64_t curCycle) override { return curCycle; }
    CommPacket* nextPacket(uint32_t fromLevel, uint32_t fromCommI, 
                           uint32_t sizeLimit) override;
    void commandLoadBalance(uint64_t curCycle) override { return; }
    void executeLoadBalance(uint32_t command, 
        std::vector<DataHotness>& outInfo) override;

    uint64_t stateLocalTaskQueueSize() override;
private:
    void handleInPacket(CommPacket* packet) override;
    bool isChild(int id) override {
        return ((uint32_t)id == this->commId);
    }
public:
    void initStats(AggregateStat* parentStat) override;

    friend class PimBridgeTaskUnit;
    friend class ReserveLbPimBridgeTaskUnit;
};

class CommModule : public CommModuleBase {
private:
    uint32_t childBeginId;
    uint32_t childEndId;
    GatherScheme* gatherScheme;
    ScatterScheme* scatterScheme;

    // state information
    uint64_t lastGatherPhase;
    uint64_t lastScatterPhase;

    // packet buffer
    std::vector<CommPacketQueue> scatterBuffer;

    std::vector<uint64_t> childQueueLength;
    std::vector<uint64_t> childTransferSize;
    std::vector<uint64_t> childQueueReadyLength;

    bool enableLoadBalance;

public:
    CommModule(uint32_t _level, uint32_t _commId, bool _enableInterflow, 
               uint32_t _childBeginId, uint32_t _childEndId, 
               GatherScheme* _gatherScheme, ScatterScheme* _scatterScheme, 
               bool _enableLoadBalance);
    
    uint64_t communicate(uint64_t curCycle) override;
    CommPacket* nextPacket(uint32_t fromLevel, uint32_t fromCommId, 
                           uint32_t sizeLimit) override;
    void gatherState() override; 
    void commandLoadBalance(uint64_t curCycle) override;
    void executeLoadBalance(uint32_t command, 
        std::vector<DataHotness>& outInfo);
    bool isEmpty() override;
    
    uint64_t stateLocalTaskQueueSize() override;
    // getters & setters
    uint64_t getLastGatherPhase() { return this->lastGatherPhase; }
    uint64_t getLastScatterPhase() { return this->lastScatterPhase; }

private: 
    void handleInPacket(CommPacket* packet) override;
    bool isChild(int id) override {
        return (id >= 0 && (uint32_t)id >= this->bankBeginId && 
            (uint32_t)id < this->bankEndId);
    }
    uint64_t gather(uint64_t curCycle);
    uint64_t scatter(uint64_t curCycle);
    bool shouldCommandLoadBalance();

public:
    void initStats(AggregateStat* parentStat) override;
private:
    Counter s_GatherTimes, s_ScatterTimes, s_GatherPackets, s_ScatterPackets;
    VectorCounter sv_GatherPackets, sv_ScatterPackets;

    friend class pimbridge::GatherScheme;
    friend class pimbridge::IntervalGather;
    friend class pimbridge::OnDemandGather;
    friend class pimbridge::OnDemandOfAllGather;
    friend class pimbridge::WheneverGather;
    friend class pimbridge::DynamicGather;
    friend class pimbridge::DynamicOnDemandGather;
    friend class pimbridge::DynamicIntervalGather;
    friend class pimbridge::TaskGenerationTrackGather;

    friend class pimbridge::AfterGatherScatter;
    friend class pimbridge::IntervalScatter;
    friend class pimbridge::OnDemandScatter;

    friend class pimbridge::LoadBalancer;
    friend class pimbridge::StealingLoadBalancer;
    friend class pimbridge::ReserveLoadBalancer;
    friend class pimbridge::AverageLoadBalancer;
};

    
} // namespace pimbridge
