#include "mem_router.h"
#include "timing_event.h"
#include "mem_interconnect_event_recorder.h"
#include "zsim.h"

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

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
    uint32_t outDelay = lastHop ? (size + bytesPerCycle - 1) / bytesPerCycle : 0;
    profTrans.atomicInc();
    profSize.atomicInc(size);
    uint64_t respCycle = cycle + procDelay + outDelay;

    // Create events.
    auto itcnRec = zinfo->memInterconnectEventRecorders[srcCoreId];
    assert(itcnRec);
    itcnRec->addHop(this, portId, procDelay, outDelay, cycle, respCycle);

    return respCycle;
}

uint64_t TimingMemRouter::simulate(uint32_t portId, uint32_t procDelay, uint32_t outDelay, bool lastHop, uint64_t startCycle) {
    // Process.
    uint64_t procStartCycle = procDisp.dispatch(startCycle);
    profQueuingProcCycles.inc(procStartCycle - startCycle);
    uint64_t procDoneCycle = procStartCycle + procDelay;
    DEBUG("%s TimingMemRouter: simProc: %lu -> %lu", name.c_str(), startCycle, procDoneCycle);

    // Output port.
    uint64_t outStartCycle = std::max(procDoneCycle, lastOutDoneCycle[portId]);
    profQueuingOutCycles.inc(portId, outStartCycle - procDoneCycle);
    lastOutDoneCycle[portId] = outStartCycle + outDelay;
    DEBUG("%s TimingMemRouter: simOut (%u): %lu -> %lu-%lu", name.c_str(), portId, procDoneCycle, outStartCycle, lastOutDoneCycle[portId]);
    return lastHop ? lastOutDoneCycle[portId] : outStartCycle;
}

