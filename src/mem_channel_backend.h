#ifndef MEM_CHANNEL_BACKEND_H_
#define MEM_CHANNEL_BACKEND_H_

#include "intrusive_list.h"
#include "memory_hierarchy.h"

class MemChannelAccEvent;

// Access request record. Linked in a linked list based on the priority.
struct MemChannelAccReq : InListNode<MemChannelAccReq> {
    Address addr;
    bool isWrite;

    // Cycle when arriving at memory, including queue overflow stalls.
    // Only used for latency stats.
    // in sys cycles.
    uint64_t startCycle;
    // Cycle when entering schedule queues. Minimum time to be issued.
    // in mem cycles.
    uint64_t schedCycle;

    // Event to response. Null for writes as they get responses immediately.
    MemChannelAccEvent* ev;
};

class MemChannelBackend {
    public:
        // Enqueue a request to the schedule queue.
        // Return the tick cycle if it can be issued (i.e., no higher-priority requests), o/w -1.
        virtual uint64_t enqueue(const Address& addr, const bool isWrite, uint64_t startCycle,
                uint64_t memCycle, MemChannelAccEvent* respEv) = 0;

        // Dequeue a request \c req to issue with tick cycle no later than \c memCycle.
        // Return if succeed. If not, set the minimum tick cycle \c minTickCycle.
        virtual bool dequeue(uint64_t memCycle, MemChannelAccReq* req, uint64_t* minTickCycle) = 0;

        virtual bool queueOverflow(const bool isWrite) const = 0;

        // Process the request. Return respond cycle.
        virtual uint64_t process(const MemChannelAccReq* req) = 0;

        // Return a lower bound of tick cycle.
        virtual uint64_t getTickCycleLowerBound() const = 0;

        virtual uint32_t getMemFreqKHz() const = 0;
        virtual uint32_t getMinLatency(const bool isWrite) const = 0;
};

#endif  // MEM_CHANNEL_BACKEND_H_
