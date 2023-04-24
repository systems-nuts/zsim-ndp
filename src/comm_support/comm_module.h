#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include "galloc.h"
#include "stats.h"
#include "task_support/task.h"
#include "comm_support/comm_packet.h"
#include "comm_support/gather_scheme.h"
#include "comm_support/scatter_scheme.h"

using namespace task_support;

namespace pimbridge {

class CommModuleBase {
protected:
    std::string name;
    uint32_t level;
    uint32_t commId;
    bool enableInterflow;

    std::deque<CommPacket*> parentPackets;
    // same-level direct communication 
    uint32_t siblingBeginId;
    uint32_t siblingEndId;
    std::vector<std::deque<CommPacket*>> siblingPackets;

    lock_t commLock;

public:
    CommModuleBase(uint32_t _level, uint32_t _commId, bool _enableInterflow);
    void initSiblings(uint32_t sibBegin, uint32_t sibEnd);

    virtual uint64_t communicate(uint64_t curCycle) = 0; 
    virtual bool isEmpty();
    uint32_t receiveMessage(std::deque<CommPacket*>* srcBuffer, 
                            uint32_t messageSize, uint64_t readyCycle);  // return number of actually received packets.

    std::deque<CommPacket*>* getParentPackets() {
        return &(this->parentPackets);
    }
    std::deque<CommPacket*>* getSiblingPackets(uint32_t sibId) {
        return &(this->siblingPackets[sibId]);
    }
    const char* getName() {
        return this->name.c_str();
    }

    virtual void initStats(AggregateStat* parentStat) {};

protected:
    bool shouldInterflow();
    void interflow(uint32_t sibId, uint32_t messageSize);
    virtual void receivePacket(CommPacket* packet) = 0;

    void handleOutPacket(CommPacket* packet); 
    bool isSibling(uint32_t id) {
        return (id >= siblingBeginId && id <= siblingEndId && id != commId);
    }
};

class PimBridgeTaskUnit;

class BottomCommModule : public CommModuleBase{
private:
    PimBridgeTaskUnit* taskUnit;

public:
    BottomCommModule(uint32_t _level, uint32_t _commId, 
                     bool _enableInterflow, PimBridgeTaskUnit* _taskUnit);
    uint64_t communicate(uint64_t curCycle) override { return curCycle; }
    void receivePacket(CommPacket* packet) override;
    void generatePacket(uint32_t dst, TaskPtr t);

    void initStats(AggregateStat* parentStat) override;
private:
    Counter s_GenPackets, s_RecvPackets;
};

class CommModule : public CommModuleBase {
private:
    uint32_t childBeginId;
    uint32_t childEndId;
    GatherScheme* gatherScheme;
    ScatterScheme* scatterScheme;

    // state information
    bool gatherJustNow;

    // packet buffer
    std::vector<std::deque<CommPacket*>> scatterBuffer;

public:
    CommModule(uint32_t _level, uint32_t _commId, bool _enableInterflow, 
               uint32_t _childBeginId, uint32_t _childEndId, 
               GatherScheme* _gatherScheme, ScatterScheme* _scatterScheme);
    void receivePacket(CommPacket* packet) override;
    
    uint64_t communicate(uint64_t curCycle) override;
    bool isEmpty() override;

    void initStats(AggregateStat* parentStat) override;

private: 
    bool inLocalModule(uint32_t loc) {
        return (loc >= this->childBeginId && loc < this->childEndId);
    }
    bool shouldGather();
    bool shouldScatter();
    uint64_t gather(uint64_t curCycle);
    uint64_t scatter(uint64_t curCycle);

    Counter s_RecvPackets, s_GatherTimes, s_ScatterTimes;
    VectorCounter sv_GatherPackets, sv_ScatterPackets;

    friend class IntervalGather;
    friend class OnDemandGather;
    friend class WheneverGather;
    friend class AfterGatherScatter;
    friend class IntervalScatter;
    friend class OnDemandScatter;
};

    
} // namespace task_support
