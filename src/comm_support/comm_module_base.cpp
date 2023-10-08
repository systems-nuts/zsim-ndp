#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "numa_map.h"
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
    this->addrRemapTable = new AddressRemapTable(level, commId);
    this->executeSpeed = 0;
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
        // if (zinfo->beginDebugOutput) {
            // info("packet: type %u, fromLevel: %u, fromComm: %u, toLevel: %u, toComm: %d, priority: %u, sig: %lu, addr: %lu", 
            //     p->type, p->fromLevel, p->fromCommId, p->toLevel, p->toCommId, 
            //     p->priority, p->getSignature(), p->getAddr());
        // }
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
    packet->fromCommId = this->commId;
    packet->fromLevel = this->level;
    if (enableInterflow && this->isSibling(packet->toCommId)) {
        uint32_t bufferId = packet->toCommId - siblingBeginId;
        packet->toLevel = this->level;
        this->siblingPackets[bufferId].push(packet);
    } else {
        packet->toLevel = this->level+1;
        packet->toCommId = -1;
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

void CommModuleBase::newAddrLend(Address lbPageAddr) {
    Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(pageAddr);
    DEBUG_SCHED_META_O("module %s lend data: %lu, nodeId: %u", this->getName(), lbPageAddr, nodeId);
    assert(!addrRemapTable->getAddrLend(lbPageAddr) && 
        !addrRemapTable->getAddrBorrowMidState(lbPageAddr));
    addrRemapTable->setChildRemap(lbPageAddr, -1);
    if (isChild(nodeId)) {
        addrRemapTable->setAddrLend(lbPageAddr, true);
    } 
    this->s_ScheduleOutData.atomicInc(1);
}

void CommModuleBase::newAddrRemap(Address lbPageAddr, uint32_t dst, bool isMidState) {
    DEBUG_SCHED_META_O("module %s receive data %lu commId: %u: isMid: %u", 
        this->getName(), lbPageAddr, this->commId, isMidState);
    Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(pageAddr);
    if (this->level == 0) {
        if (isChild(nodeId)) {
            assert(nodeId == this->commId);
            if (isMidState) {
                addrRemapTable->setAddrBorrowMidState(lbPageAddr, 0);
            } else if (addrRemapTable->getAddrBorrowMidState(lbPageAddr)) {
                addrRemapTable->eraseAddrBorrowMidState(lbPageAddr);
            }
            addrRemapTable->setAddrLend(lbPageAddr, false);
        } else {
            assert(!addrRemapTable->getAddrLend(lbPageAddr));
            assert(addrRemapTable->getChildRemap(lbPageAddr) == -1);
            if (isMidState) {
                addrRemapTable->setAddrBorrowMidState(lbPageAddr, 0);
            } else {
                if (addrRemapTable->getAddrBorrowMidState(lbPageAddr)) {
                    addrRemapTable->eraseAddrBorrowMidState(lbPageAddr);
                }
                addrRemapTable->setChildRemap(lbPageAddr, dst);
            }
        }
        this->s_ScheduleInData.atomicInc(1);
    } else {
        assert(!isMidState);
        if (isChild(nodeId)) {
            if (addrRemapTable->getAddrLend(lbPageAddr)) {
                addrRemapTable->setAddrLend(lbPageAddr, false);
            }
            uint32_t childCommId = zinfo->commMapping->getCommId(level-1, nodeId);
            if (childCommId != dst) {
                addrRemapTable->setChildRemap(lbPageAddr, dst);
            } else {
                addrRemapTable->setChildRemap(lbPageAddr, -1);
            }
        } else {
            assert(!addrRemapTable->getAddrLend(lbPageAddr));
            addrRemapTable->setChildRemap(lbPageAddr, dst);
        }
    }
}