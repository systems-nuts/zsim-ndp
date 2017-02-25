#include "mem_router.h"
#include "timing_event.h"

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

/* Events */

class MemRouterProcEvent : public TimingEvent {
    private:
        TimingMemRouter* router;
        const uint64_t id;

        friend class TimingMemRouter;

    public:
        MemRouterProcEvent(TimingMemRouter* _router, uint64_t _id, uint32_t postDelay, int32_t domain)
            : TimingEvent(0, postDelay, domain), router(_router), id(_id) {}

        void simulate(uint64_t startCycle) {
            router->simProc(this, startCycle);
        }
};

class MemRouterOutEvent : public TimingEvent {
    private:
        TimingMemRouter* router;
        const uint64_t id;
        const uint32_t portId;
        const uint32_t occCycles;

        friend class TimingMemRouter;

    public:
        MemRouterOutEvent(TimingMemRouter* _router, uint64_t _id, uint32_t _portId, uint32_t _occCycles, uint32_t postDelay, int32_t domain)
            : TimingEvent(0, postDelay, domain), router(_router), id(_id), portId(_portId), occCycles(_occCycles) {}

        void simulate(uint64_t startCycle) {
            router->simOutPort(this, startCycle);
        }
};


void TimingMemRouter::initStats(AggregateStat* parentStat) {
    AggregateStat* routerStat = initBaseStats();

    profQueuingProcCycles.init("queuingProcCycles", "Queuing cycles for processing");
    profQueuingOutCycles.init("queuingOutCycles", "Queuing cycles for output ports", numPorts);
    routerStat->append(&profQueuingProcCycles);
    routerStat->append(&profQueuingOutCycles);

    parentStat->append(routerStat);
}

uint64_t TimingMemRouter::transfer(uint64_t cycle, uint64_t size, uint32_t portId, bool lastHop, uint32_t srcCoreId) {
    // Bound phase delays.
    uint32_t procDelay = latency;
    uint32_t outDelay = (size + bytesPerCycle - 1) / bytesPerCycle;
    uint64_t procDoneCycle = cycle + procDelay;
    uint64_t outDoneCycle = procDoneCycle + outDelay;

    profTrans.atomicInc();
    profSize.atomicInc(size);

    uint64_t respCycle = lastHop ? outDoneCycle : procDoneCycle;

    // FIXME(mgao): ignored, see mem_interconnect.cpp.
    if (unlikely(srcCoreId == -1u)) return respCycle;

    EventRecorder* evRec = zinfo->eventRecorders[srcCoreId];
    if (evRec == nullptr) panic("TimingMemRouter: must be connected to core with timing model");

    // Create events.
    // Two arbitrations: process (routing calc, xbar, etc., shared by all ports) and output (per port).
    auto procEv = new (evRec) MemRouterProcEvent(this, evId, procDelay, domain);
    procEv->setMinStartCycle(cycle);
    auto outEv = new (evRec) MemRouterOutEvent(this, evId, portId, outDelay, lastHop ? outDelay : 0, domain);
    outEv->setMinStartCycle(procDoneCycle);
    procEv->addChild(outEv, evRec);
    evId++;

    // Record events.
    assert(evRec->hasRecord());  // MemInterconnect must create the start event.
    auto tr = evRec->popRecord();
    assert(tr.endEvent);  // end event is either a GET or an interconnect event.
    // Concatenate events by chaining at the end.
    assert(tr.respCycle <= cycle);
    auto dEv = new (evRec) DelayEvent(cycle - tr.respCycle);
    dEv->setMinStartCycle(tr.respCycle);
    tr.endEvent->addChild(dEv, evRec)->addChild(procEv, evRec);  // outEv has already been chained.
    tr.respCycle = respCycle;
    tr.endEvent = outEv;
    evRec->pushRecord(tr);

    return respCycle;
}

void TimingMemRouter::simProc(MemRouterProcEvent* ev, uint64_t cycle) {
    uint64_t procStartCycle = procDisp.dispatch(cycle);
    profQueuingProcCycles.inc(procStartCycle - cycle);
    ev->done(procStartCycle);
    DEBUG("%s TimingMemRouter: %lu, simProc: %lu -> %lu", name.c_str(), ev->id, cycle, procStartCycle);
}

void TimingMemRouter::simOutPort(MemRouterOutEvent* ev, uint64_t cycle) {
    auto portId = ev->portId;
    uint64_t outStartCycle = std::max(cycle, lastOutDoneCycle[portId]);
    lastOutDoneCycle[portId] = outStartCycle + ev->occCycles;
    profQueuingOutCycles.inc(portId, outStartCycle - cycle);
    ev->done(outStartCycle);  // postDelay includes serialize delay.
    DEBUG("%s TimingMemRouter: %lu, simOut: %lu -> %lu (%lu)", name.c_str(), ev->id, cycle, outStartCycle, lastOutDoneCycle[portId]);
}

