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
    this->childTopItemLength.resize(childEndId - childBeginId);
}

uint64_t CommModule::communicate(uint64_t curCycle) {
    uint64_t respCycle = curCycle;
    // info("resp before gather: %lu", respCycle);
    if (this->gatherScheme->shouldTrigger()) {
        respCycle = gather(respCycle);
    }
    // info("resp after gather: %lu", respCycle);
    if (this->scatterScheme->shouldTrigger()) {
        respCycle = scatter(respCycle);
    }
    // info("resp after scatter: %lu", respCycle);
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

void CommModule::commandLoadBalance() {
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
    // info("%s execute load balance", this->getName());
    this->loadBalancer->generateCommandFromUpper(command);
    uint64_t curOutSize = outInfo.size();
    for (uint32_t i = this->childBeginId; i < this->childEndId; ++i) { 
        uint32_t curCommand = loadBalancer->commands[i-childBeginId];
        if (curCommand > 0) {
            zinfo->commModules[level-1][i]->executeLoadBalance(curCommand, outInfo);
        }
    }
    for (uint64_t i = curOutSize; i < outInfo.size(); ++i) {
        this->newAddrLend(outInfo[i].addr);
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

uint64_t CommModule::stateReadyLength() {
    uint64_t res = 0;
    for (uint32_t i = childBeginId; i < childEndId; ++i) {
        res += this->childQueueReadyLength[i-childBeginId];
    }
    return res;
}

uint64_t CommModule::stateAllLength() {
    uint64_t res = 0;
    for (uint32_t i = childBeginId; i < childEndId; ++i) {
        res += this->childQueueLength[i-childBeginId];
    }
    return res;
}

uint64_t CommModule::stateTopItemLength() {
    return 0; 
}

void CommModule::handleInPacket(CommPacket* packet) {
    // if (packet->getInnerType() == CommPacket::DataLend && packet->from == 4) {
    //     info("addr: %lu, orig packet to: %d", packet->getAddr(), packet->to);
    // }
    assert(packet->toLevel == this->level);
    int avail = this->checkAvailable(packet->getAddr());
    if (avail == -1) {
        this->handleOutPacket(packet);
    } else {
        assert(avail >= 0);
        uint32_t availLoc = (uint32_t)avail;
        this->handleToChildPacket(packet, availLoc);
    }
    s_RecvPackets.atomicInc(1);
}

void CommModule::handleToChildPacket(CommPacket* packet, uint32_t childCommId) {
    packet->fromLevel = this->level;
    packet->fromCommId = this->commId;
    packet->toLevel = this->level - 1;
    packet->toCommId = childCommId;
    this->scatterBuffer[childCommId - childBeginId].push(packet);
}

int CommModule::checkAvailable(Address lbPageAddr) {
    Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(pageAddr);
    int remap = this->addrRemapTable->getChildRemap(lbPageAddr);
    if (remap != -1) {
        assert(!this->addrRemapTable->getAddrLend(lbPageAddr));
        return remap;
    } else {
        assert(!this->addrRemapTable->getAddrLend(lbPageAddr) || isChild(nodeId));
        if (isChild(nodeId) && !this->addrRemapTable->getAddrLend(lbPageAddr)) {
            return zinfo->commMapping->getCommId(this->level-1, nodeId);
        } else {
            return -1;
        }
    }
}

uint64_t CommModule::gather(uint64_t curCycle) {
    // info("gather: %u-%u", level, commId);
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            uint64_t respCycle = zinfo->cores[i]->recvCommReq(true, curCycle, i, 
                (this->gatherScheme->packetSize - 64));
                // 8);
            // info("resp of %u: %lu", i, respCycle);
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
    // if(zinfo->beginDebugOutput) {
        // info("scatter: %u-%u", level, commId);
    // }
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            uint64_t respCycle = zinfo->cores[i]->recvCommReq(false, curCycle, 
                i, this->scatterScheme->packetSize);
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

        uint64_t newChildLength = child->stateAllLength();
        uint64_t newChildReadyLength = child->stateReadyLength();

        this->childQueueLength[id] = newChildLength;
        this->childQueueReadyLength[id] = newChildReadyLength;
        this->childTransferSize[id] = child->stateTransferRegionSize();
        this->childTopItemLength[id] = child->stateTopItemLength();

#ifdef DEBUG_GATHER_STATE
        info("module %s length: %lu readyLength: %lu notReadyLength: %lu, transferLength: %lu", 
            child->getName(), newChildLength, newChildReadyLength, 
            newChildLength-newChildReadyLength,
            childTransferSize[id]);
#endif
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
        } else if (childQueueReadyLength[i-childBeginId] >= 2 * loadBalancer->IDLE_THRESHOLD){
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

    s_ScheduleOutData.init("scheduleOutData", "Number of scheduled out data");
    commStat->append(&s_ScheduleOutData);
    s_ScheduleInData.init("scheduleInData", "Number of scheduled in data");
    commStat->append(&s_ScheduleInData);
    s_ScheduleOutTasks.init("scheduleOutTasks", "Number of scheduled out tasks");
    commStat->append(&s_ScheduleOutTasks);
    s_ScheduleInTasks.init("scheduleInTasks", "Number of scheduled in tasks");
    commStat->append(&s_ScheduleInTasks);

    uint32_t numChild = childEndId - childBeginId;
    sv_GatherPackets.init("gatherPacketsPerChild", "Number of gathered packets per child", numChild);
    commStat->append(&sv_GatherPackets);
    sv_ScatterPackets.init("scatterPacketsPerChild", "Number of scattered packets per child", numChild);
    commStat->append(&sv_ScatterPackets);

    parentStat->append(commStat);
}