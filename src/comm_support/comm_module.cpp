#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "comm_support/comm_module.h"
#include "comm_support/comm_mapping.h"
#include "gather_scheme.h"
#include "scatter_scheme.h"
#include "numa_map.h"

using namespace pimbridge;
using namespace task_support;


CommModule::CommModule(uint32_t _level, uint32_t _commId, bool _enableInterflow, 
                       uint32_t _childBeginId, uint32_t _childEndId, 
                       GatherScheme* _gatherScheme, 
                       ScatterScheme* _scatterScheme, 
                       bool _enableLoadBalance) 
    : CommModuleBase(_level, _commId, _enableInterflow), 
      childBeginId(_childBeginId), childEndId(_childEndId), 
      gatherScheme(_gatherScheme),scatterScheme(_scatterScheme), 
      lastGatherPhase(0), lastScatterPhase(0), 
      enableLoadBalance(_enableLoadBalance) {
    info("---build comm module: childBegin: %u, childEnd: %u", childBeginId, childEndId);
    assert(this->level > 0);
    this->bankBeginId = zinfo->commModules[level-1][childBeginId]->getBankBeginId();
    this->bankEndId = zinfo->commModules[level-1][childEndId-1]->getBankEndId();
    zinfo->commMapping->setMapping(level, bankBeginId, bankEndId, commId);
    info("begin Id: %u, endId: %u", bankBeginId, bankEndId);
    this->scatterBuffer.resize(childEndId - childBeginId);
    gatherScheme->setCommModule(this);
    scatterScheme->setCommModule(this);
    this->childQueueLength.resize(childEndId - childBeginId);
    this->childTransferSize.resize(childEndId - childBeginId);
    this->childQueueReadyLength.resize(childEndId - childBeginId);
}

uint64_t CommModule::communicate(uint64_t curCycle) {
    uint64_t respCycle = curCycle;
    if (this->gatherScheme->shouldTrigger()) {
        respCycle = gather(respCycle);
    }
    if (this->scatterScheme->shouldTrigger()) {
        respCycle = scatter(respCycle);
    }
    return respCycle;
}

CommPacket* CommModule::nextPacket(uint32_t fromLevel, uint32_t fromCommId, 
                                   uint32_t sizeLimit) {
    CommPacketQueue* cpd = nullptr;
    if (fromLevel == this->level - 1) {
        // scatter
        cpd = &(this->scatterBuffer[fromCommId - childBeginId]);
    } else if (fromLevel == this->level) {
        // interflow
        cpd = &(this->siblingPackets[fromCommId - siblingBeginId]);
    } else if (fromLevel == this->level + 1) {
        // gather
        cpd = &(this->parentPackets);
    } else {
        panic("invalid fromLevel %u for nextPacket from CommModule", fromLevel);
    }
    CommPacket* ret = cpd->front();
    if (ret != nullptr && ret->getSize() < sizeLimit) {
        cpd->pop();
        return ret;
    }
    return nullptr;
}

void CommModule::commandLoadBalance(uint64_t curCycle) {
    if (!this->shouldCommandLoadBalance()) {
        return;
    }
    this->loadBalancer->generateCommand();
    // The information of scheduled out data
    // write in executeLoadBalance (by lb executors)
    // read in assignLbTarget (by lb commanders)
    std::vector<DataHotness> outInfo;
    outInfo.clear();
    for (uint32_t i = this->childBeginId; i < this->childEndId; ++i) {
        assert(loadBalancer->commands[i-childBeginId] == 0 || loadBalancer->needs[i-childBeginId] == 0);
        uint32_t curCommand = loadBalancer->commands[i-childBeginId];
        if (curCommand > 0) {
            zinfo->commModules[level-1][i]->executeLoadBalance(curCommand, outInfo);
        }
    }
    this->loadBalancer->assignLbTarget(outInfo);   
}

void CommModule::executeLoadBalance(uint32_t command, 
        std::vector<DataHotness>& outInfo) {
    this->loadBalancer->generateCommandFromUpper(command);
    for (uint32_t i = this->childBeginId; i < this->childEndId; ++i) { 
        uint32_t curCommand = loadBalancer->commands[i-childBeginId];
        if (curCommand > 0) {
            zinfo->commModules[level-1][i]->executeLoadBalance(curCommand, outInfo);
        }
    }
}

bool CommModule::isEmpty() {
    if (!CommModuleBase::isEmpty()) {
        return false;
    }
    for (auto pb : this->scatterBuffer) {
        if (!pb.empty()) { return false; }
    }
    return true;
}

uint64_t CommModule::stateLocalTaskQueueSize() {
    uint64_t res = 0;
    for (uint32_t i = childBeginId; i < childEndId; ++i) {
        res += zinfo->commModules[level-1][i]->stateLocalTaskQueueSize();
    }
    return res;
}

void CommModule::handleInPacket(CommPacket* packet) {
    if (this->addrRemapTable->getAddrLend(packet->getAddr())) {
        packet->to = -1;
    } else {
        int remap = this->addrRemapTable->getChildRemap(packet->getAddr());
        if (remap != -1) {
            packet->to = remap;
        } /*else {
            // not lend, not remap, then it is in the original location. 
            Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(packet->getAddr());
            assert((uint32_t)packet->to == zinfo->numaMap->getNodeOfPage(pageAddr));
        }*/
    } 
    // Notice that it is possible that to == from
    // This happens when a unit lends a data and then borrows it back
    // assert(packet->to >= 0 /*&& packet->to != packet->from*/);
    // Notice that packet->to can still be less than 0 with cross-rank scheduling.
    if (isChild(packet->to)) {
        uint32_t bufferId = zinfo->commMapping->getCommId(this->level-1, (uint32_t)packet->to) - childBeginId;
        this->scatterBuffer[bufferId].push(packet);
    } else {
        this->handleOutPacket(packet);
        this->s_GenPackets.atomicInc(1);
    }
    s_RecvPackets.atomicInc(1);
}

uint64_t CommModule::gather(uint64_t curCycle) {
    // info("gather: %u-%u", level, commId);
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            uint64_t respCycle = zinfo->cores[i]->recvCommReq(true, curCycle, i);
            readyCycle = respCycle > readyCycle ? respCycle : readyCycle;
        }
    }

    zinfo->gatherProfiler->initTransfer(this->level, this->commId);

    for (size_t i = childBeginId; i < childEndId; ++i) {
        CommModuleBase* src = zinfo->commModules[this->level-1][i];
        uint32_t numPackets = 0, totalSize = 0;
        uint32_t packetSize = this->gatherScheme->packetSize;
        
        this->receivePackets(src, packetSize, readyCycle, numPackets, totalSize);
        this->sv_GatherPackets.atomicInc(i-childBeginId, numPackets);
        this->s_GatherPackets.atomicInc(numPackets);
        zinfo->gatherProfiler->record(this->level, this->commId, i-childBeginId, totalSize);
    }

    this->lastGatherPhase = zinfo->numPhases;
    this->s_GatherTimes.atomicInc(1);
    return readyCycle;
}

uint64_t CommModule::scatter(uint64_t curCycle) {
    // info("scatter: %u-%u", level, commId);
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            uint64_t respCycle = zinfo->cores[i]->recvCommReq(true, curCycle, i);
            readyCycle = respCycle > readyCycle ? respCycle : readyCycle;
        }
    }
    for (size_t i = childBeginId; i < childEndId; ++i) {
        uint32_t numPackets = 0, totalSize = 0;
        zinfo->commModules[level-1][i]->
            receivePackets(this, this->scatterScheme->packetSize, readyCycle, 
                           numPackets, totalSize);
        this->sv_ScatterPackets.atomicInc(i-childBeginId, numPackets);
        this->s_GatherPackets.atomicInc(numPackets);
    }
    this->s_ScatterTimes.atomicInc(1);
    this->lastScatterPhase = zinfo->numPhases;
    return readyCycle;
}

void CommModule::gatherState() {
    for (uint32_t i = childBeginId; i < childEndId; ++i) {
        CommModuleBase* child = zinfo->commModules[level-1][i];
        uint32_t id = i - childBeginId;
        this->childQueueReadyLength[id] = child->stateLocalTaskQueueSize();
        this->childQueueLength[id] = child->stateLocalTaskQueueSize() + 
            child->stateToStealSize();
        this->childTransferSize[id] = child->stateTransferRegionSize();
    }
    this->loadBalancer->updateChildStateForLB();
}

bool CommModule::shouldCommandLoadBalance() {
    if (!this->enableLoadBalance) {
        return false;
    }
    bool hasIdle = false, hasNotIdle = false;
    for (uint32_t i = childBeginId; i < childEndId; ++i) {
        if (childQueueLength[i-childBeginId] < loadBalancer->IDLE_THRESHOLD) {
            hasIdle = true;
        } else if (childQueueReadyLength[i-childBeginId] >= loadBalancer->IDLE_THRESHOLD){
            hasNotIdle = true;
        }
    }
    return (hasIdle && hasNotIdle);
}

void CommModule::initStats(AggregateStat* parentStat) {
    AggregateStat* commStat = new AggregateStat();
    commStat->init(name.c_str(), "Communication module stats");

    s_GenTasks.init("genTasks", "Number of generated tasks");
    commStat->append(&s_GenTasks);
    s_FinishTasks.init("finishTasks", "Number of finished tasks");
    commStat->append(&s_FinishTasks);

    s_GenPackets.init("genPackets", "Number of generated packets");
    commStat->append(&s_GenPackets);
    s_RecvPackets.init("recvPackets", "Number of received packets");
    commStat->append(&s_RecvPackets);

    s_GatherTimes.init("gatherTimes", "Number of gathering");
    commStat->append(&s_GatherTimes);
    s_GatherPackets.init("gatherPackets", "Number of gathered packets");
    commStat->append(&s_GatherPackets);
    s_ScatterTimes.init("scatterTimes", "Number of scattering");
    commStat->append(&s_ScatterTimes);
    s_ScatterPackets.init("scatterPackets", "Number of scattered packets");
    commStat->append(&s_ScatterPackets);

    uint32_t numChild = childEndId - childBeginId;
    sv_GatherPackets.init("gatherPacketsPerChild", "Number of gathered packets per child", numChild);
    commStat->append(&sv_GatherPackets);
    sv_ScatterPackets.init("scatterPacketsPerChild", "Number of scattered packets per child", numChild);
    commStat->append(&sv_ScatterPackets);

    parentStat->append(commStat);
}