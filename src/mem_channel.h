#ifndef MEM_CHANNEL_H_
#define MEM_CHANNEL_H_

#include <deque>
#include "finite_queue.h"
#include "g_std/g_string.h"
#include "intrusive_list.h"
#include "mem_channel_backend.h"
#include "memory_hierarchy.h"
#include "stats.h"

class MemChannelAccEvent;
class MemChannelTickEvent;

/**
 * A generic memory channel weave model.
 *
 * The model accepts memory access request \c MemReq in bound phase and returns minimum latency.
 * In weave phase, it schedules and issues the requests at certain "tick" time based on the timing
 * and priority defined by specific backend model.
 *
 * This class takes care of the weave phase simulation event recording and management. The specfic
 * timing behaviors and scheduling algorithm are defined in backend model.
 */
class MemChannel : public MemObject {
    public:
        MemChannel(/*TODO*/);

        const char* getName() { return name.c_str(); }

        void initStats(AggregateStat* parentStat);

        /* Bound phase. */
        uint64_t access(MemReq& req);

        /* Weave phase. */
        void acceptAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle);
        void respondAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle);

        uint64_t tick(uint64_t sysCycle);
        void recycleTickEvent(MemChannelTickEvent* tev);

    private:
        const g_string name;
        const uint32_t domain;

        // Backend of the channel.
        MemChannelBackend* be;

        /* Controller timing. */

        // Bound phase delays, in sys cycles.
        uint32_t minRdDelay, minWrDelay, preRdDelay, preWrDelay, postRdDelay, postWrDelay;

        uint64_t sysFreqKHz;
        uint64_t memFreqKHz;

        // sys <-> mem cycle translation.
        // We receive and return system cycles, but all internal logic is in memory cycles.
        // The +1 ensures that we never go back in time when translating, i.e., sync across clock
        // domain makes event be handled in the next cycle.
        inline uint64_t sysToMemCycle(uint64_t sysCycle) const {
            return sysCycle * memFreqKHz / sysFreqKHz + 1;
        }
        inline uint64_t memToSysCycle(uint64_t memCycle) const {
            return memCycle * sysFreqKHz / memFreqKHz + 1;
        }
        // Ensure sysToMemCycle(matchingSysCycle(memCycle)) == memCycle.
        inline uint64_t matchingSysCycle(uint64_t memCycle) const {
            // mC <= sC*mF/sF+1 < mC+1  ==>  (mC-1)*sF/mF <= sC < mC*sF/mF
            // So sC is the max int that less than mC*sF/mF (no equal), i.e., (mC*sF-1/2)/mF.
            return (memCycle * sysFreqKHz * 2 - 1) / 2 / memFreqKHz;
        }

        /* Scheduling. */

        // Tick cycle for the next event, in mem cycles.
        uint64_t tickCycle;
        // Tick event.
        MemChannelTickEvent* tickEvent;
        // Keep one free tick event for fast alloc/dealloc.
        MemChannelTickEvent* freeTickEvent;

        // Overflow request queue, not considered when scheduling.
        // Record AccEvent and startCycle.
        std::deque<std::pair<MemChannelAccEvent*, uint64_t>> overflowQueue;

        /* Stats. */


    private:
        // Schedule event \c ev started at \c startCycle, at current time \c memCycle.
        // Return the estimated tick time for this access request in mem cycle.
        uint64_t schedule(MemChannelAccEvent* ev, uint64_t startCycle, uint64_t memCycle);

        // Try to issue a request whose tick time is no later than current time \c memCycle.
        // Return the next tick time in mem cycle.
        uint64_t issue(uint64_t memCycle);

};

#endif  // MEM_CHANNEL_H_
