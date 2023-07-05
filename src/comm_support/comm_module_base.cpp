#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "comm_support/comm_module.h"
#include "comm_support/comm_mapping.h"
#include "comm_support/comm_packet_queue.h"
#include "gather_scheme.h"
#include "scatter_scheme.h"

using namespace pimbridge;
using namespace task_support;


CommModuleBase::CommModuleBase(uint32_t _level, uint32_t _commId, 
                               bool _enableInterflow)
    : level(_level), commId(_commId), enableInterflow(_enableInterflow) {
    std::stringstream ss; 
    ss << "comm-" << level << "-" << commId;
    this->name = std::string(ss.str());
    futex_init(&commLock); 
    this->parentId = (uint32_t)-1;
    this->toStealSize = 0;
    this->addrRemapTable = new AddressRemapTable(level, commId);
}

void CommModuleBase::initSiblings(uint32_t sibBegin, uint32_t sibEnd) {
    assert(this->enableInterflow);
    this->siblingBeginId = sibBegin;
    this->siblingEndId = sibEnd;
    this->siblingPackets.resize(sibEnd - sibBegin);
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

void CommModuleBase::receivePackets(CommModuleBase* src, 
                                    uint32_t messageSize, uint64_t readyCycle, 
                                    uint32_t& numPackets, uint32_t& totalSize) {
    totalSize = 0;
    numPackets = 0;
    while(true) {
        CommPacket* p = src->nextPacket(this->level, this->commId, messageSize - totalSize);
        if (p == nullptr) {
            // if the size is not enough, also return nullptr
            break;
        }
        p->readyCycle = readyCycle;
        totalSize += p->getSize();
        this->handleInPacket(p);
        numPackets += 1;
        assert(messageSize >= totalSize);
        if (totalSize == messageSize) {
            break;
        }
    }                              
}

void CommModuleBase::handleOutPacket(CommPacket* packet) {
    if (enableInterflow && this->isSibling(packet->to)) {
        uint32_t bufferId = zinfo->commMapping->getCommId(this->level, packet->to) - siblingBeginId;
        this->siblingPackets[bufferId].push(packet);
    } else {
        this->parentPackets.push(packet);
    } 
    this->s_GenPackets.atomicInc(1);
}

uint64_t CommModuleBase::stateTransferRegionSize() {
    return this->parentPackets.getSize();
}

void CommModuleBase::interflow(uint32_t sibId, uint32_t messageSize) {
    uint32_t numPackets = 0, totalSize = 0; 
    zinfo->commModules[this->level][sibId]->receivePackets(
            this, messageSize, 0/*TBY TODO: readyCycle*/, 
            numPackets, totalSize);
}

void CommModuleBase::addToSteal(uint32_t num) {
    this->toStealSize += num;
}

// void CommModuleBase::finishTask() {
//     this->s_FinishTasks.atomicInc(1);
//     if (this->parentId != (uint32_t)-1) {
//         zinfo->commModules[level+1][parentId]->finishTask();
//     }
// }
// void CommModuleBase::generateTask() {
//     this->s_GenTasks.atomicInc(1);
//     if (this->parentId != (uint32_t)-1) {
//         zinfo->commModules[level+1][parentId]->generateTask();
//     }
// }