#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "comm_support/comm_module.h"

using namespace pimbridge;
using namespace task_support;

CommModuleBase::CommModuleBase(uint32_t _level, uint32_t _commId, 
                               bool _enableInterflow)
    : level(_level), commId(_commId), enableInterflow(_enableInterflow) {
    std::stringstream ss; 
    ss << "comm-" << level << "-" << commId;
    this->name = std::string(ss.str());
    futex_init(&commLock); 
}

void CommModuleBase::initSiblings(uint32_t sibBegin, uint32_t sibEnd) {
    assert(this->enableInterflow);
    this->siblingBeginId = sibBegin;
    this->siblingEndId = sibEnd;
    this->siblingPackets.resize(sibEnd - sibBegin);
}

void CommModuleBase::interflow(uint32_t sibId, uint32_t messageSize) {
    zinfo->commModules[this->level][sibId]->receiveMessage(
        this->getSiblingPackets(sibId), messageSize, 0/*TBY TODO: readyCycle*/);
}

uint32_t CommModuleBase::receiveMessage(std::deque<CommPacket*>* parentBuffer, 
                                    uint32_t messageSize, uint64_t readyCycle) {
    uint32_t totalSize = 0;
    uint32_t numPackets = 0;
    while(!parentBuffer->empty()) {
        CommPacket* p = parentBuffer->front();
        parentBuffer->pop_front();
        p->readyCycle = readyCycle;
        this->receivePacket(p);
        numPackets += 1;
        totalSize += p->getSize();
        if (totalSize >= messageSize) {
            break;
        }
    }
    return numPackets;                                   
}

bool CommModuleBase::isEmpty() {
    if (!this->parentPackets.empty()) {
        return false;
    }
    if (this->enableInterflow) {
        for (auto pb : this->siblingPackets) {
            if (!pb.empty()) { return false; }
        }
    }
    return true;
}

void CommModuleBase::handleOutPacket(CommPacket* packet) {
    if (enableInterflow && this->isSibling(packet->to)) {
        this->siblingPackets[packet->to].push_back(packet);
    } else {
        this->parentPackets.push_back(packet);
    }
}

BottomCommModule::BottomCommModule(uint32_t _level, uint32_t _commId, 
                                   bool _enableInterflow, PimBridgeTaskUnit* _taskUnit)
    : CommModuleBase(_level, _commId, _enableInterflow), taskUnit(_taskUnit) {
    assert(taskUnit->getTaskUnitId() == this->commId);
    this->taskUnit->setCommModule(this);
}

void BottomCommModule::receivePacket(CommPacket* packet) {
    packet->task->readyCycle = packet->readyCycle;
    this->taskUnit->taskEnqueue(packet->task);
    delete packet;
    s_RecvPackets.atomicInc(1);
}

void BottomCommModule::generatePacket(uint32_t dst, TaskPtr t) {
    CommPacket* p = new CommPacket(0, this->commId, dst, t);
    this->handleOutPacket(p);
    s_GenPackets.atomicInc(1);
}

void BottomCommModule::initStats(AggregateStat* parentStat) {
    AggregateStat* commStat = new AggregateStat();
    commStat->init(name.c_str(), "Communication module stats");

    s_GenPackets.init("genPackets", "Number of generated packets");
    commStat->append(&s_GenPackets);
    s_RecvPackets.init("recvPackets", "Number of received packets");
    commStat->append(&s_RecvPackets);

    parentStat->append(commStat);
}

CommModule::CommModule(uint32_t _level, uint32_t _commId, bool _enableInterflow, 
                       uint32_t _childBeginId, uint32_t _childEndId, 
                       GatherScheme* _gatherScheme, 
                       ScatterScheme* _scatterScheme) 
    : CommModuleBase(_level, _commId, _enableInterflow), 
      childBeginId(_childBeginId), childEndId(_childEndId), 
      gatherScheme(_gatherScheme),scatterScheme(_scatterScheme), 
      gatherJustNow(false) {
    info("---build comm module: childBegin: %u, childEnd: %u", childBeginId, childEndId);
    this->scatterBuffer.resize(childEndId - childBeginId);
}

void CommModule::receivePacket(CommPacket* packet) {
    this->scatterBuffer[packet->to - childBeginId].push_back(packet);
    s_RecvPackets.atomicInc(1);
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

uint64_t CommModule::communicate(uint64_t curCycle) {
    uint64_t respCycle = curCycle;
    if (this->shouldGather()) {
        respCycle = gather(respCycle);
    }
    if (this->shouldScatter()) {
        respCycle = scatter(respCycle);
    }
    return respCycle;
}

uint64_t CommModule::gather(uint64_t curCycle) {
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        MESIState dummyState = MESIState::I;
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            // TBY TODO: srcId should be set to host core. Currently, we set srcId = i, which is very fragile!
            MemReq req = {0, GETS, i, &dummyState, curCycle, nullptr, dummyState, i, 0}; 
            uint64_t respCycle = zinfo->toMemEndpoints[i]->forgeAccess(req, i);
            readyCycle = respCycle > readyCycle ? respCycle : readyCycle;
        }
    }

    for (size_t i = childBeginId; i < childEndId; ++i) {
        std::deque<CommPacket*>* buffer = 
            zinfo->commModules[this->level-1][i]->getParentPackets();
        uint32_t totalSize = 0;
        while(!buffer->empty()) {
            CommPacket* p = buffer->front();
            assert(p->readyCycle <= curCycle);
            p->readyCycle = readyCycle;
            buffer->pop_front();
            // info("gather packet: from: %u, to: %u, taskId: %u", p->from, p->to, p->task->taskId);
            assert(inLocalModule(p->from));
            if (inLocalModule(p->to)) {
                this->scatterBuffer[p->to - childBeginId].push_back(p);
            } else {
                this->handleOutPacket(p);
            }
            totalSize += p->getSize();
            this->sv_GatherPackets.atomicInc(i-childBeginId, 1);
            if (totalSize >= this->gatherScheme->packetSize) {
                break;
            }
        }
    }
    this->gatherJustNow = true;
    this->s_GatherTimes.atomicInc(1);
    return readyCycle;
}

uint64_t CommModule::scatter(uint64_t curCycle) {
    // TBY TODO: simulate gather events
    uint64_t readyCycle = curCycle;
    if (this->level == 1) {
        MESIState dummyState = MESIState::I;
        for (uint32_t i = childBeginId; i < childEndId; ++i) {
            // TBY TODO: srcId should be set to host core. Currently, we set srcId = i, which is very fragile!
            MemReq req = {0, GETX, i, &dummyState, curCycle, nullptr, dummyState, i, 0}; 
            uint64_t respCycle = zinfo->toMemEndpoints[i]->forgeAccess(req, i);
            readyCycle = respCycle > readyCycle ? respCycle : readyCycle;
        }
    }

    for (size_t i = childBeginId; i < childEndId; ++i) {
        uint32_t numPackets = zinfo->commModules[level-1][i]->
            receiveMessage(&(this->scatterBuffer[i-childBeginId]), 
                           this->scatterScheme->packetSize, readyCycle);
        this->sv_ScatterPackets.atomicInc(i-childBeginId, numPackets);
    }
    this->s_ScatterTimes.atomicInc(1);
    return readyCycle;
}

bool CommModule::shouldGather() { 
    return gatherScheme->shouldTrigger(this); 
}

bool CommModule::shouldScatter() {
    bool ret = scatterScheme->shouldTrigger(this);
    this->gatherJustNow = false;
    return ret;
}

void CommModule::initStats(AggregateStat* parentStat) {
    AggregateStat* commStat = new AggregateStat();
    commStat->init(name.c_str(), "Communication module stats");

    s_GatherTimes.init("gatherTimes", "Number of gathering");
    commStat->append(&s_GatherTimes);
    s_ScatterTimes.init("scatterTimes", "Number of scattering");
    commStat->append(&s_ScatterTimes);

    s_RecvPackets.init("recvPackets", "Number of received packets");
    commStat->append(&s_RecvPackets);
    uint32_t numChild = childEndId - childBeginId;
    sv_GatherPackets.init("gatherPackets", "Number of gathered packets", numChild);
    commStat->append(&sv_GatherPackets);
    sv_ScatterPackets.init("scatterPackets", "Number of scattered packets", numChild);
    commStat->append(&sv_ScatterPackets);

    parentStat->append(commStat);

}
