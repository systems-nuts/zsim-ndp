#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "comm_support/comm_module.h"
#include "comm_support/comm_mapping.h"
#include "gather_scheme.h"
#include "scatter_scheme.h"

using namespace pimbridge;
using namespace task_support;

BottomCommModule::BottomCommModule(uint32_t _level, uint32_t _commId, 
                                   bool _enableInterflow, PimBridgeTaskUnit* _taskUnit)
    : CommModuleBase(_level, _commId, _enableInterflow), taskUnit(_taskUnit) {
    bankBeginId = commId;
    bankEndId = commId + 1;
    info("begin Id: %u, endId: %u", bankBeginId, bankEndId);
    zinfo->commMapping->setMapping(level, bankBeginId, bankEndId, commId);
    taskUnit->setCommModule(this);
    assert(taskUnit->getTaskUnitId() == this->commId);;
}

CommPacket* BottomCommModule::nextPacket(uint32_t fromLevel, uint32_t fromCommId, 
                                         uint32_t sizeLimit) {
    CommPacketQueue* cpd;
    if (fromLevel == 0) {
        cpd = &(this->siblingPackets[fromCommId-siblingBeginId]);
    } else if (fromLevel == 1) {
        cpd = &(this->parentPackets);
    } else {
        panic("invalid fromLevel %u for nextPacket from BottomCommModule", fromLevel);
    }
    CommPacket* ret = cpd->front();
    if (ret != nullptr && ret->getSize() < sizeLimit) {
        cpd->pop();
        return ret;
    }
    return nullptr;
}

void BottomCommModule::executeLoadBalance(uint32_t command, 
        std::vector<DataHotness>& outInfo) {
    this->taskUnit->executeLoadBalanceCommand(command, outInfo);
}

uint64_t BottomCommModule::stateLocalTaskQueueSize() {
    return this->taskUnit->getTaskQueueSize();
}

void BottomCommModule::handleInPacket(CommPacket* packet) {
    assert(packet->to >= 0 && (uint32_t)packet->to == this->commId);
    if (packet->type == CommPacket::PacketType::Sub) {
        auto p = (SubCommPacket*) packet; 
        if (p->parent->type == CommPacket::PacketType::DataLend) {
            // info("handle in dataLend: from %u, to %u, %lu, id: %u", 
            //     packet->from, packet->to, p->getAddr(), p->idx);
            this->addrRemapTable->setAddrBorrowMidState(p->getAddr(), p->idx);
        }
        if (p->isLast()) {
            p->parent->to = this->commId;
            this->handleInPacket(p->parent);
            delete packet;
            return; // should not add recvPackets, since we have done it for the parent packet
        }
    } else if (packet->type == CommPacket::PacketType::Task) {
        auto p = (TaskCommPacket*) packet;
        if (p->forLb()) {
            if (this->toStealSize >= 1) {
                --this->toStealSize;
            }
            // assert(toStealSize >= 1);
        }
        p->task->readyCycle = p->readyCycle;
        this->taskUnit->taskEnqueue(p->task);
    } else if (packet->type == CommPacket::PacketType::DataLend) {
        auto p = (DataLendCommPacket*) packet;
        this->taskUnit->newAddrBorrow(p->getAddr());
    } else {
        panic("Invalid packet type %u for BottomCommModule!", packet->type);
    }
    delete packet;
    s_RecvPackets.atomicInc(1);
}

void BottomCommModule::initStats(AggregateStat* parentStat) {
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

    parentStat->append(commStat);
}