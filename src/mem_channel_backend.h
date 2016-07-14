#ifndef MEM_CHANNEL_BACKEND_H_
#define MEM_CHANNEL_BACKEND_H_

#include "intrusive_list.h"
#include "memory_hierarchy.h"

class MemChannelAccEvent;

// Access request record.
struct MemChannelAccReq : public GlobAlloc {
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

    virtual ~MemChannelAccReq() {}

    // Use glob mem
    using GlobAlloc::operator new;
    using GlobAlloc::operator delete;
};

class MemChannelBackend {
    public:
        // Enqueue a request to the schedule queue.
        // Return the tick cycle if it can be issued (i.e., no higher-priority requests), o/w -1.
        virtual uint64_t enqueue(const Address& addr, const bool isWrite, uint64_t startCycle,
                uint64_t memCycle, MemChannelAccEvent* respEv) = 0;

        // Dequeue a request \c req to issue with tick cycle no later than \c memCycle.
        // Return if succeed. If not, set the minimum tick cycle \c minTickCycle.
        virtual bool dequeue(uint64_t memCycle, MemChannelAccReq** req, uint64_t* minTickCycle) = 0;

        virtual bool queueOverflow(const bool isWrite) const = 0;

        // Process the request. Return respond cycle.
        virtual uint64_t process(const MemChannelAccReq* req) = 0;

        // Return a lower bound of tick cycle.
        virtual uint64_t getTickCycleLowerBound() const = 0;

        virtual uint32_t getMemFreqKHz() const = 0;
        virtual uint32_t getMinLatency(const bool isWrite) const = 0;

        virtual void initStats(AggregateStat* parentStat) {}
};

/**
 * A simple channel backend with fixed latency for all access, and pure age-based priority.
 */
class MemChannelBackendSimple : public MemChannelBackend {
    public:
        MemChannelBackendSimple(const uint32_t _freqMHz, const uint32_t _latency, const uint32_t _queueDepth)
            : freqMHz(_freqMHz), latency(_latency), queueDepth(_queueDepth), lastRespCycle(0)
        {
            reqQueue.init(queueDepth);
        }

        uint64_t enqueue(const Address& addr, const bool isWrite, uint64_t startCycle,
                uint64_t memCycle, MemChannelAccEvent* respEv) {
            auto req = reqQueue.alloc();
            req->addr = addr;
            req->isWrite = isWrite;
            req->startCycle = startCycle;
            req->schedCycle = memCycle;
            req->ev = respEv;

            // If there is only one request, i.e., the one just enqueued, it has highest priority.
            if (reqQueue.size() == 1) return req->schedCycle + latency;
            else return -1uL;
        }

        bool dequeue(uint64_t memCycle, MemChannelAccReq** req, uint64_t* minTickCycle) {
            if (reqQueue.empty()) {
                *minTickCycle = -1uL;
                return false;
            }
            auto begin = reqQueue.begin();
            auto front = *begin;
            uint64_t tickCycle = front->schedCycle + latency;
            if (tickCycle > memCycle) {
                *minTickCycle = tickCycle;
                return false;
            }
            *req = new MemChannelAccReq(*front);
            reqQueue.remove(begin);
            return true;
        }

        bool queueOverflow(const bool isWrite) const {
            return reqQueue.full();
        }

        uint64_t process(const MemChannelAccReq* req) {
            uint64_t respCycle = req->schedCycle + latency;
            respCycle = std::max(respCycle, lastRespCycle + 1);
            lastRespCycle = respCycle;
            return respCycle;
        }

        uint64_t getTickCycleLowerBound() const {
            uint64_t nextTickCycle = reqQueue.empty() ? -1uL : (*reqQueue.begin())->schedCycle + latency;
            return std::max(lastRespCycle + 1, nextTickCycle);
        }

        uint32_t getMemFreqKHz() const { return freqMHz * 1000; }
        uint32_t getMinLatency(const bool isWrite) const { return latency; }

    private:
        // Frequency.
        uint32_t freqMHz;
        // Fix latency.
        uint32_t latency;

        // Request queue.
        uint32_t queueDepth;
        FiniteQueue<MemChannelAccReq> reqQueue;

        // Last respond cycle.
        uint64_t lastRespCycle;
};

#endif  // MEM_CHANNEL_BACKEND_H_
