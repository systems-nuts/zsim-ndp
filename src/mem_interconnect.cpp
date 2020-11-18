#include "mem_interconnect.h"
#include <sstream>
#include "log.h"
#include "mem_interconnect_event_recorder.h"
#include "mem_router.h"
#include "routing_algorithm.h"
#include "zsim.h"

#define INTERCONNECT_MAX_HOPS 100 /* avoid livelock */

MemInterconnect::MemInterconnect(RoutingAlgorithm* _ra, const g_vector<MemRouter*>& _routers, uint32_t _ccHeaderSize, const g_string& _name)
    : ra(_ra), routers(_routers), numTerminals(_ra->getNumTerminals()), ccHeaderSize(_ccHeaderSize), name(_name)
{
    assert(ra->getNumRouters() == routers.size());

    needsCSim = false;
    for (auto& r : routers) needsCSim |= r->needsCSim();
}

uint64_t MemInterconnect::accessRequest(const MemReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId) {
    uint64_t size = ccHeaderSize;  // request
    if (req.type == PUTX) size += (1 << lineBits);  // data

    auto itcnRec = zinfo->memInterconnectEventRecorders[req.srcId];

    if (needsCSim) itcnRec->startRequest<true>(cycle, req.lineAddr, req.type);
    cycle = travel(srcId, dstId, size, cycle, req.srcId);
    if (needsCSim) itcnRec->endRequest<true>(cycle);

    return cycle;
}

uint64_t MemInterconnect::accessResponse(const MemReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId) {
    uint64_t size = ccHeaderSize;  // acknowledgment or permission
    if (req.type == GETS || (req.type == GETX && req.initialState == I)) size += (1 << lineBits);  // data

    auto itcnRec = zinfo->memInterconnectEventRecorders[req.srcId];

    if (needsCSim) cycle = itcnRec->startResponse<true>(cycle);
    cycle = travel(srcId, dstId, size, cycle, req.srcId);
    if (needsCSim) itcnRec->endResponse<true>(cycle);

    return cycle;
}

uint64_t MemInterconnect::invalidateRequest(const InvReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId) {
    uint64_t size = ccHeaderSize;  // request
    if (req.type == FWD) size += (1 << lineBits);  // data

    auto itcnRec = zinfo->memInterconnectEventRecorders[req.srcId];

    if (needsCSim) itcnRec->startRequest<false>(cycle);
    cycle = travel(srcId, dstId, size, cycle, req.srcId);
    if (needsCSim) itcnRec->endRequest<false>(cycle);

    return cycle;
}

uint64_t MemInterconnect::invalidateResponse(const InvReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId) {
    uint64_t size = ccHeaderSize;  // acknowledgment
    // NOTE(gaomy): with a broadcast cc hub, req.writeback could be nullptr, and inv filter does not help here as it is behind interconnect.
    if (req.writeback && *req.writeback) size += (1 << lineBits);  // data written back

    auto itcnRec = zinfo->memInterconnectEventRecorders[req.srcId];

    if (needsCSim) cycle = itcnRec->startResponse<false>(cycle);
    cycle = travel(srcId, dstId, size, cycle, req.srcId);
    if (needsCSim) itcnRec->endResponse<false>(cycle);

    return cycle;
}

uint64_t MemInterconnect::travel(uint32_t srcId, uint32_t dstId, size_t size, uint64_t cycle, uint32_t srcCoreId) {
    assert(srcId < numTerminals);
    assert(dstId < numTerminals);

    uint64_t respCycle = cycle;
    uint32_t nhops = 0;
    uint32_t curId = srcId;
    while (curId != dstId && nhops < INTERCONNECT_MAX_HOPS) {
        uint32_t nextId = -1;
        uint32_t portId = -1;
        ra->nextHop(curId, dstId, &nextId, &portId);
        assert(nextId < ra->getNumRouters());
        assert(portId < ra->getNumPorts());
        respCycle = routers[curId]->transfer(respCycle, size, portId, nextId == dstId, srcCoreId);
        curId = nextId;
        nhops++;
    }
    if (nhops >= INTERCONNECT_MAX_HOPS) {
        panic("[mem_interconnect] Routing from %u to %u takes more than %u hops!", srcId, dstId, INTERCONNECT_MAX_HOPS);
    }

    return respCycle;
}

