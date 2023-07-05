#pragma once
#include <deque>
#include <queue>
#include "comm_support/comm_packet.h"

namespace pimbridge {

struct packetCmp {
    bool operator()(const CommPacket* p1, const CommPacket* p2) {
        if (p1->priority != p2->priority) {
            return p1->priority > p2->priority;
        } else if (p1->readyCycle != p2->readyCycle) {
            return p1->readyCycle > p2->readyCycle;
        } else if (p1->getIdx() != p2->getIdx()) {
            return p1->getIdx() > p2->getIdx();
        } else {
            return p1->getSignature() > p2->getSignature();
        }
    }
};

// The main reason for using CommPacketQueue is that the sizes of packets vary
// CommPacketQueue maintains sizes inside. 

class CommPacketQueue {
private:    
    uint64_t size;
    // std::deque<CommPacket*> pdq;
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
        if (p->getSize() > CommPacket::MAX_SIZE) {
            // use (a+b-1)/b to get ceil(a/b)
            uint32_t total = (p->getSize() + CommPacket::MAX_SIZE - 1) / CommPacket::MAX_SIZE;
            for (uint32_t i = 0; i < total; ++i) {
                SubCommPacket* sp = new SubCommPacket(p, i, total);
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
    bool empty() {
        return pdq.empty();
    }
    uint64_t getSize() {
        return this->size;
    }

};

}