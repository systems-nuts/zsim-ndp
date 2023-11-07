#pragma once
#include <deque>
#include <queue>
#include "comm_support/comm_packet.h"
#include "debug_output.h"

namespace pimbridge {

struct packetCmp {
    bool operator()(const CommPacket* p1, const CommPacket* p2) {
        /*
        if (p1->timeStamp != p2->timeStamp) {
            return p1->timeStamp > p2->timeStamp;
        } else if (p1->readyCycle != p2->readyCycle) {
            return p1->readyCycle > p2->readyCycle;
        } else if (p1->priority != p2->priority) {
            return p1->priority < p2->priority; // normal task first
        } else if (p1->getSignature() != p2->getSignature()) {
            return p1->getSignature() > p2->getSignature();
        } else if (p1->getInnerType() != p2->getInnerType()) {
            return p1->getInnerType() < p2->getInnerType(); // data first, then task
        } else if (p1->getIdx() != p2->getIdx()) {
            return  p1->getIdx() > p2->getIdx();
        } else {
            assert_msg(false, "Two totally same packet! sig: %lu, addr: %lu, %lu \n"\
                "p1: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p2: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p1 addr: %lu, p2 addr: %lu\n",
                p1->getSignature(),p1->getAddr(),p2->getAddr(), 
                p1->type, p1->fromLevel, p1->fromCommId, 
                p1->toLevel, p1->toCommId, p1->priority, p1->getIdx(), p1->getInnerType(),
                p2->type, p2->fromLevel, p2->fromCommId, 
                p2->toLevel, p2->toCommId, p2->priority, p2->getIdx(), p2->getInnerType(), 
                (uint64_t)p1, (uint64_t)p2);
        }*/

        if (p1->timeStamp != p2->timeStamp) {
            return p1->timeStamp > p2->timeStamp;
        } else if (p1->readyCycle != p2->readyCycle) {
            return p1->readyCycle > p2->readyCycle;
        } else if (p1->priority != p2->priority) {
            return p1->priority < p2->priority; // normal task first
        } else if (p1->getAddr() != p2->getAddr()) {
            return p1->getAddr() > p2->getAddr();
        } else if (p1->getInnerType() != p2->getInnerType()) {
            return p1->getInnerType() < p2->getInnerType();
        } else if (p1->getSignature() != p2->getSignature()) {
            return p1->getSignature() > p2->getSignature();
        } else if (p1->getIdx() != p2->getIdx()) {
            return  p1->getIdx() > p2->getIdx();
        } else {
            assert_msg(false, "Two totally same packet! sig: %lu, addr: %lu, %lu \n"\
                "p1: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p2: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p1 addr: %lu, p2 addr: %lu\n",
                p1->getSignature(),p1->getAddr(),p2->getAddr(), 
                p1->type, p1->fromLevel, p1->fromCommId, 
                p1->toLevel, p1->toCommId, p1->priority, p1->getIdx(), p1->getInnerType(),
                p2->type, p2->fromLevel, p2->fromCommId, 
                p2->toLevel, p2->toCommId, p2->priority, p2->getIdx(), p2->getInnerType(), 
                (uint64_t)p1, (uint64_t)p2);
        }
        /*
        if (p1->timeStamp != p2->timeStamp) {
            return p1->timeStamp > p2->timeStamp;
        } else if (p1->priority != p2->priority) {
            return p1->priority < p2->priority; // normal task first
        } else if (p1->readyCycle != p2->readyCycle) {
            return p1->readyCycle > p2->readyCycle;
        } else if (p1->getInnerType() != p2->getInnerType()) {
            return p1->getInnerType() < p2->getInnerType();
        } else if (p1->getSignature() != p2->getSignature()) {
            return p1->getSignature() > p2->getSignature();
        } else if (p1->getIdx() != p2->getIdx()) {
            return  p1->getIdx() > p2->getIdx();
        } else {
            assert_msg(false, "Two totally same packet! sig: %lu, addr: %lu, %lu \n"\
                "p1: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p2: type: %u, from: %u-%u, to: %u-%u, prio: %u, idx: %u, innertype: %u\n"\
                "p1 addr: %lu, p2 addr: %lu\n",
                p1->getSignature(),p1->getAddr(),p2->getAddr(), 
                p1->type, p1->fromLevel, p1->fromCommId, 
                p1->toLevel, p1->toCommId, p1->priority, p1->getIdx(), p1->getInnerType(),
                p2->type, p2->fromLevel, p2->fromCommId, 
                p2->toLevel, p2->toCommId, p2->priority, p2->getIdx(), p2->getInnerType(), 
                (uint64_t)p1, (uint64_t)p2);
        }*/
    }
};

// The main reason for using CommPacketQueue is that the sizes of packets vary
// CommPacketQueue maintains sizes inside. 

class CommPacketQueue {
private:    
    uint64_t size;
    std::priority_queue<CommPacket*, std::deque<CommPacket*>, packetCmp> pdq;
public:
    CommPacketQueue() : size(0) {}
    void pop() {
        assert(!pdq.empty());
        CommPacket* p = pdq.top();
        pdq.pop();
        this->size -= p->getSize();
    }
    void push(CommPacket* p) {
        // if (p->getInnerType() == 1) {
        //     DEBUG_ADDR_RETURN_O("push: packet addr: %lu, idx: %u", p->addr, p->getIdx());
        // }
        if (p->getSize() > CommPacket::MAX_SIZE) {
            // tby: use (a+b-1)/b to get ceil(a/b)
            uint32_t total = (p->getSize() + CommPacket::MAX_SIZE - 1) / CommPacket::MAX_SIZE;
            for (uint32_t i = 0; i < total; ++i) {
                SubCommPacket* sp = new SubCommPacket(p, i+1, total);
                pdq.push(sp);
                this->size += sp->getSize();
            }
        } else {
            pdq.push(p);
            this->size += p->getSize();
        }
    }
    CommPacket* front() {
        if (pdq.empty()) {
            return nullptr;
        }
        return this->pdq.top();
    }
    bool empty(uint64_t ts) {
        if (this->empty()) {
            return true;
        }
        if (ts != 0 && this->front()->timeStamp > ts) {
            return true;
        } 
        return false;
    }
    bool empty() {
        return pdq.empty();
    }
    uint64_t getSize() {
        return this->size;
    }

};

}