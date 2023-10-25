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
    PacketType innerType;
    uint64_t timeStamp;
    uint64_t readyCycle;
    uint32_t fromLevel;
    uint32_t fromCommId;
    uint32_t toLevel;
    int toCommId; 
    uint32_t priority;

    uint64_t size;
    Address addr;
    uint64_t signature;
    
    CommPacket(PacketType _type, PacketType _innerType,
               uint32_t _timeStamp, uint64_t _readyCycle, 
               uint32_t _fromLevel, uint32_t _fromCommId, 
               uint32_t _toLevel, int _toCommId, 
               uint32_t _priority) 
        : type(_type), timeStamp(_timeStamp), readyCycle(_readyCycle), 
          fromLevel(_fromLevel), fromCommId(_fromCommId), 
          toLevel(_toLevel), toCommId(_toCommId), priority(_priority) {}

    virtual ~CommPacket() {}
    uint64_t getSize() const {
        return size;
    }
    Address getAddr() const {
        return addr;
    }
    virtual uint32_t getIdx() const {
        return 0;
    }
    uint64_t getSignature() const {
        return signature;
    }
    PacketType getInnerType() const {
        return this->innerType;
    }
};

class TaskCommPacket : public CommPacket{
public:
    task_support::TaskPtr task;
    // priority = 3 means this is a normal transfer packet
    // priority = 2 means this is a packet for load balance
    TaskCommPacket(uint32_t _timeStamp, uint64_t _readyCycle,
                   uint32_t _fromLevel, uint32_t _fromCommId, 
                   uint32_t _toLevel, int _toCommId,
                   task_support::TaskPtr _task, uint32_t _priority = 3);
    bool forLb() const {
        return this->priority == 2;
    }
};

class DataLendCommPacket : public CommPacket {
public: 
    DataLendCommPacket(uint32_t _timeStamp, uint64_t _readyCycle, 
                       uint32_t _fromLevel, uint32_t _fromCommId, 
                       uint32_t _toLevel, int _toCommId,
                       Address _lbPageAddr, uint32_t _dataSize)
        : CommPacket(PacketType::DataLend, PacketType::DataLend, 
                    _timeStamp, _readyCycle, 
                    _fromLevel, _fromCommId, _toLevel, _toCommId, 2) {
        this->size = _dataSize;
        this->addr = _lbPageAddr;
        this->signature = _lbPageAddr;
    }
};


// TBY: Some packets may be large.
// To make sure that a packet can be transferred inside one gather, 
// we limit the max packet size to CommPacket::MAX_SIZE. 
// So the large packets will be divided into multiple SubCommPackets. 
// The 1st - (total-1)th packets will not be handled in the HandleInPacket
// For the (total)th packet (isLast() = true), we process the parent packet. 
// The divide is done in CommPacketQueue
class SubCommPacket : public CommPacket {
public:
    CommPacket* parent;
    uint32_t idx; // start with 1;
    uint32_t total;
    SubCommPacket(CommPacket* _parent, uint32_t _idx, uint32_t _total) 
        : CommPacket(PacketType::Sub, _parent->type,
                     _parent->timeStamp, _parent->readyCycle, 
                     _parent->fromLevel, _parent->fromCommId, 
                     _parent->toLevel, _parent->toCommId, _parent->priority), 
          parent(_parent), idx(_idx), total(_total) {
        this->size = CommPacket::MAX_SIZE;
        this->addr = this->parent->getAddr();
        this->signature = this->parent->getSignature();
    }
    uint32_t getIdx() const override {
        return idx;
    }
    bool isLast() {
        return (idx == total);
    }
};

}