#ifndef MEM_ROUTER_H_
#define MEM_ROUTER_H_

#include "g_std/g_string.h"
#include "locks.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "zsim.h"

class MemRouter : public GlobAlloc {
    protected:
        const uint32_t numPorts;

        Counter profTrans;
        Counter profSize;

        const g_string name;

    public:
        MemRouter(uint32_t _numPorts, const g_string& _name) : numPorts(_numPorts), name(_name) {}
        const char* getName() const { return name.c_str(); }
        uint32_t getNumPorts() const { return numPorts; }

        virtual void initStats(AggregateStat* parentStat) {
            parentStat->append(initBaseStats());
        }

        /* Bound phase. */

        virtual uint64_t transfer(uint64_t cycle, uint64_t size, uint32_t portId, bool lastHop, bool piggyback, uint32_t srcCoreId) = 0;

        /* Weave phase. */

        virtual bool needsCSim() const { return false; }

        virtual uint64_t simulate(uint32_t portId, uint32_t procDelay, uint32_t outDelay, bool lastHop, bool piggyback, uint64_t startCycle) { panic("%s: not implemented!", name.c_str()); }

    protected:
        AggregateStat* initBaseStats() {
            AggregateStat* routerStat = new AggregateStat();
            routerStat->init(name.c_str(), "Router stats");

            profTrans.init("trans", "Transfers");
            profSize.init("size", "Total transferred data");
            routerStat->append(&profTrans);
            routerStat->append(&profSize);

            return routerStat;
        }
};

/**
 * Fixed latency for all ports. No contention.
 */
class SimpleMemRouter : public MemRouter {
    private:
        const uint64_t latency;

    public:
        SimpleMemRouter(uint32_t numPorts, uint64_t _latency, const g_string& name)
            : MemRouter(numPorts, name), latency(_latency) {}

        uint64_t transfer(uint64_t cycle, uint64_t size, uint32_t portId, bool lastHop, bool piggyback, uint32_t srcCoreId) {
            if (!piggyback) profTrans.atomicInc();
            profSize.atomicInc(size);
            return cycle + latency;
        }
};

/**
 * Router with limited bandwidth for ports and throttling latency based on M/D/1 queueing model.
 */
class MD1MemRouter : public MemRouter {
    private:
        const uint64_t latency;
        const uint32_t bytesPerCycle;  // per port

        // Use for coarse-grained queuing latency factor update.
        lock_t updateLock;
        uint32_t lastPhase;
        g_vector<uint32_t> queuingFactorsX100;
        g_vector<uint64_t> curTransData;
        g_vector<uint64_t> smoothedTransData;

        Counter profClampedLoads;
        VectorCounter profLoadHist;  // 10% bin

    public:
        MD1MemRouter(uint32_t numPorts, uint64_t _latency, uint32_t _bytesPerCycle, const g_string& name)
            : MemRouter(numPorts, name), latency(_latency), bytesPerCycle(_bytesPerCycle),
              lastPhase(0), queuingFactorsX100(numPorts, 100), curTransData(numPorts, 0),
              smoothedTransData(numPorts, 0)
        {
            futex_init(&updateLock);
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* routerStat = initBaseStats();

            profClampedLoads.init("clampedLoads", "Number of transfers with load clamped to 95%");
            profLoadHist.init("loadHists", "Load histogram (10\% bin)", 10);
            routerStat->append(&profClampedLoads);
            routerStat->append(&profLoadHist);

            parentStat->append(routerStat);
        }

        uint64_t transfer(uint64_t cycle, uint64_t size, uint32_t portId, bool lastHop, bool piggyback, uint32_t srcCoreId) {
            assert(portId < numPorts);

            // Update queuing factors.
            // Double-checked locking.
            if (zinfo->numPhases > lastPhase) {
                futex_lock(&updateLock);
                if (zinfo->numPhases > lastPhase) {
                    updateQueuingFactors();
                    // Need barrier here, see http://www.aristeia.com/Papers/DDJ_Jul_Aug_2004_revised.pdf
                    __sync_synchronize();
                    lastPhase = zinfo->numPhases;
                }
                futex_unlock(&updateLock);
            }

            __sync_fetch_and_add(&curTransData[portId], size);
            if (!piggyback) profTrans.atomicInc();
            profSize.atomicInc(size);

            uint32_t serializeDelay = (size + bytesPerCycle - 1) / bytesPerCycle;
            uint32_t queuingDelay = size / bytesPerCycle * queuingFactorsX100[portId] / 100;
            return cycle + latency + queuingDelay + (lastHop ? serializeDelay : 0);
        }

    private:
        void updateQueuingFactors() {
            uint32_t phaseCycles = (zinfo->numPhases - lastPhase) * zinfo->phaseLength;
            if (phaseCycles < 10000) return; //Skip with short phases

            for (uint32_t portId = 0; portId < numPorts; portId++) {
                smoothedTransData[portId] = (curTransData[portId] + smoothedTransData[portId]) / 2;
                curTransData[portId] = 0;
                uint32_t load = 100 * smoothedTransData[portId] / phaseCycles / bytesPerCycle;
                // Clamp load
                if (load > 95) {
                    load = 95;
                    profClampedLoads.inc();
                }
                profLoadHist.inc(load / 10);
                queuingFactorsX100[portId] = 50 * load / (100 - load);
            }
        }
};


class MemRouterProcEvent;
class MemRouterOutEvent;

/**
 * Router timing model with limited bandwidth for ports.
 */
class TimingMemRouter : public MemRouter {
    private:
        class ProcDispatcher : public GlobAlloc {
            private:
                uint64_t lastCycle;

                uint32_t cnt;
                const uint32_t width;

            public:
                explicit ProcDispatcher(uint32_t _width) : lastCycle(0), cnt(0), width(_width) {}

                inline uint64_t dispatch(uint64_t cycle) {
                    if (cycle > lastCycle) {
                        lastCycle = cycle;
                        cnt = 0;
                    }
                    cnt++;
                    if (cnt > width) {
                        lastCycle++;
                        cnt -= width;
                    }
                    return lastCycle;
                }
        };

    private:
        const uint64_t latency;
        const uint32_t bytesPerCycle;  // per port

        // Weave phase.
        ProcDispatcher procDisp;
        g_vector<uint64_t> lastOutDoneCycle;

        // Stats.
        Counter profQueuingProcCycles;
        VectorCounter profQueuingOutCycles;

        int32_t domain;

    public:
        TimingMemRouter(uint32_t numPorts, uint64_t _latency, uint32_t _bytesPerCycle, uint32_t _processWidth, const g_string& name, const int32_t _domain)
            : MemRouter(numPorts, name), latency(_latency), bytesPerCycle(_bytesPerCycle),
              procDisp(_processWidth), lastOutDoneCycle(numPorts, 0), domain(_domain)
        {}

        void initStats(AggregateStat* parentStat);

        uint64_t transfer(uint64_t cycle, uint64_t size, uint32_t portId, bool lastHop, bool piggyback, uint32_t srcCoreId);

        bool needsCSim() const { return true; }

        uint64_t simulate(uint32_t portId, uint32_t procDelay, uint32_t outDelay, bool lastHop, bool piggyback, uint64_t startCycle);
};

#endif  // MEM_ROUTER_H_

