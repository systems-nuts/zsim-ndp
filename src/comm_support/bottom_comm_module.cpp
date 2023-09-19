#include <deque>
#include "stats.h"
#include "core.h"
#include "zsim.h"
#include "numa_map.h"
#include "comm_support/comm_module.h"
#include "comm_support/comm_mapping.h"
#include "gather_scheme.h"
#include "scatter_scheme.h"
#include "task_support/pim_bridge_task_unit.h"

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
    this->toStealSize = 0;
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
    // info("%s execute load balance", this->getName());
    uint64_t curOutSize = outInfo.size();
    this->taskUnit->getCurUnit()->executeLoadBalanceCommand(command, outInfo);
    for (size_t i = curOutSize; i < outInfo.size(); ++i) {
        this->newAddrLend(outInfo[i].addr);
    }
}

uint64_t BottomCommModule::stateReadyLength() {
    return this->taskUnit->getCurUnit()->getTaskQueueSize();
}

uint64_t BottomCommModule::stateAllLength() {
    return this->taskUnit->getCurUnit()->getTaskQueueSize() + 
        this->toStealSize + 
        ((PimBridgeTaskUnitKernel*)this->taskUnit->getCurUnit())->notReadyTaskNumber;
}

uint64_t BottomCommModule::stateTopItemLength() {
    // return 0;
    return this->taskUnit->getCurUnit()->getTopItemLength();
}

void BottomCommModule::handleInPacket(CommPacket* packet) {
    assert_msg(packet->fromLevel == 1 && packet->toLevel == 0, "fromLevel: %u, toLevel: %u", 
        packet->fromLevel, packet->toLevel);
    assert(packet->toCommId >= 0 && (uint32_t)packet->toCommId == this->commId);
    if (packet->type == CommPacket::PacketType::Sub) {
        auto p = (SubCommPacket*) packet; 
        if (p->parent->type == CommPacket::PacketType::DataLend) {
            this->addrRemapTable->setAddrBorrowMidState(p->getAddr(), p->idx);
        }
        if (p->isLast()) {
            p->parent->fromLevel = p->fromLevel;
            p->parent->fromCommId = p->fromCommId;
            p->parent->toLevel = p->toLevel;
            p->parent->toCommId = p->toCommId;
            this->handleInPacket(p->parent);
            delete packet;
            return; // should not add recvPackets, since we have done it for the parent packet
        }
    } else if (packet->type == CommPacket::PacketType::Task) {
        int avail = this->checkAvailable(packet->getAddr());
        if (avail == -1) {
            this->handleOutPacket(packet);
            return;
        } else {
            auto p = (TaskCommPacket*) packet;
            if (p->forLb()) {
                this->s_ScheduleInTasks.atomicInc(1); 
                // info("module %s receive lb task, addr: %lu, toSteal: %lu, sig: %lu", 
                //     this->getName(), p->getAddr(), this->toStealSize, p->getSignature());
                if (this->toStealSize >= 1) {
                    --this->toStealSize;
                }
            }
            p->task->readyCycle = p->readyCycle;
            this->taskUnit->taskEnqueue(p->task, avail);
        }
    } else if (packet->type == CommPacket::PacketType::DataLend) {
        auto p = (DataLendCommPacket*) packet;
        this->newAddrRemap(p->getAddr(), 0);
        this->taskUnit->newAddrBorrow(p->getAddr());
    } else {
        panic("Invalid packet type %u for BottomCommModule!", packet->type);
    }
    delete packet;
    s_RecvPackets.atomicInc(1);
}

int BottomCommModule::checkAvailable(Address lbPageAddr) {
    Address pageAddr = zinfo->numaMap->getPageAddressFromLbPageAddress(lbPageAddr);
    uint32_t nodeId = zinfo->numaMap->getNodeOfPage(pageAddr);
    int remap = this->addrRemapTable->getChildRemap(lbPageAddr);
    if (remap != -1) {
        assert(nodeId != this->commId && !this->addrRemapTable->getAddrBorrowMidState(lbPageAddr));
        return 0;
    } else if (this->addrRemapTable->getAddrBorrowMidState(lbPageAddr)) {
        return -2;
    } else if (nodeId == this->commId && !this->addrRemapTable->getAddrLend(lbPageAddr)) {
        return 0;
    } else {
        return -1;
    }
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

    s_ScheduleOutData.init("scheduleOutData", "Number of scheduled out data");
    commStat->append(&s_ScheduleOutData);
    s_ScheduleInData.init("scheduleInData", "Number of scheduled in data");
    commStat->append(&s_ScheduleInData);
    // s_InAndOutData.init("inAndOutData", "Number of scheduled in then out data");
    // tuStat->append(&s_InAndOutData);
    // s_OutAndInData.init("outAndInData", "Number of scheduled out then in data");
    // tuStat->append(&s_OutAndInData);
    s_ScheduleOutTasks.init("scheduleOutTasks", "Number of scheduled out tasks");
    commStat->append(&s_ScheduleOutTasks);
    s_ScheduleInTasks.init("scheduleInTasks", "Number of scheduled in tasks");
    commStat->append(&s_ScheduleInTasks);

    parentStat->append(commStat);
}