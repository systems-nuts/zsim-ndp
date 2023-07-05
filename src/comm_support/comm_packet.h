#pragma once
#include <cstdint>
#include "task_support/task.h"
#include "memory_hierarchy.h"

namespace pimbridge {

class CommPacket {
public:
    const static uint32_t MAX_SIZE = 8;
    enum PacketType {
        Task, 
        DataLend, 
        Sub
    };
    PacketType type;
    uint32_t level;
    uint32_t from;
    int to; 
    uint64_t readyCycle;
    uint32_t priority;
    
    CommPacket(PacketType _type, uint32_t _level, uint32_t _from, int _to, 
               uint32_t _priority) 
        : type(_type), level(_level), from(_from), to(_to), 
          readyCycle(0), priority(_priority) { 
        assert(from != (uint32_t)to); 
    }

    virtual ~CommPacket() {}
    virtual uint64_t getSize() = 0;
    virtual Address getAddr() = 0;
    virtual uint32_t getIdx() const {
        return 0;
    }
    virtual uint64_t getSignature() const = 0;
};

class TaskCommPacket : public CommPacket{
public:
    task_support::TaskPtr task;
    // priority = 3 means this is a normal transfer packet
    // priority = 2 means this is a packet for load balance
    TaskCommPacket(uint32_t _level, uint32_t _from, int _to,
                   task_support::TaskPtr _task, uint32_t _priority = 3)
        : CommPacket(PacketType::Task, _level, _from, _to, _priority), 
          task(_task) {}
    
    uint64_t getSize() override {
        return this->task->taskSize;
    }
    Address getAddr() override;
    uint64_t getSignature() const override {
        return task->taskId;
    }
    bool forLb() {
        return (this->priority == 2);
    }
};

class DataLendCommPacket : public CommPacket {
public: 
    Address lbPageAddr;
    uint32_t dataSize;
    DataLendCommPacket(uint32_t _level, uint32_t _from, int _to,
                       Address _lbPageAddr, uint32_t _dataSize)
        : CommPacket(PacketType::DataLend ,_level, _from, _to, 1), 
          lbPageAddr(_lbPageAddr), dataSize(_dataSize){}

    uint64_t getSize() override {
        return dataSize;
    }
    Address getAddr() override {
        return this->lbPageAddr;
    }
    uint64_t getSignature() const override {
        return lbPageAddr;
    }
};


// TBY: Some packets may be large.
// To make sure that a packet can be transferred inside one gather, 
// we limit the max packet size to CommPacket::MAX_SIZE. 
// So the large packets will be divided into multiple SubCommPackets. 
// The 0 - total-2 packets will not be handled in the HandleInPacket
// For the total-1 packet (isLast() = true), we process the parent packet. 
class SubCommPacket : public CommPacket {
public:
    CommPacket* parent;
    uint32_t idx;
    uint32_t total;
    SubCommPacket(CommPacket* _parent, uint32_t _idx, uint32_t _total) : 
        CommPacket(PacketType::Sub, _parent->level, _parent->from, 
                   _parent->to, _parent->priority), 
        parent(_parent), idx(_idx), total(_total) {}
    
    uint64_t getSize() override {
        return CommPacket::MAX_SIZE;
    }
    Address getAddr() override {
        return this->parent->getAddr();
    }
    bool isLast() {
        return (idx == total - 1);
    }
    uint32_t getIdx() const override {
        return idx;
    }
    uint64_t getSignature() const override {
        return parent->getSignature();
    }
};

}