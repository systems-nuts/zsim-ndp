#include "mem_interconnect.h"
#include <sstream>
#include "address_map.h"
#include "log.h"
#include "mem_router.h"
#include "routing_algorithm.h"
#include "timing_event.h"
#include "zsim.h"

#define INTERCONNECT_MAX_HOPS 100 /* avoid livelock */

#define IDIVC(x, y) (((x) + (y) - 1) / (y))

MemInterconnect::MemInterconnect(RoutingAlgorithm* _ra, CoherentParentMap* _pm, const g_vector<MemRouter*>& _routers,
        uint32_t numParents, uint32_t numChildren, bool _centralizedParents,
        uint32_t _ccHeaderSize, bool _ignoreInvLatency, const g_string& _name)
    : ra(_ra), pm(_pm), routers(_routers), numTerminals(_ra->getNumTerminals()),
      centralizedParents(_centralizedParents), ccHeaderSize(_ccHeaderSize), ignoreInvLatency(_ignoreInvLatency),
      numParentsPerTerminal(IDIVC(numParents, numTerminals)), numTerminalsPerParent(IDIVC(numTerminals, numParents)),
      numChildrenPerTerminal(IDIVC(numChildren, numTerminals)), numTerminalsPerChild(IDIVC(numTerminals, numChildren)),
      name(_name)
{
    if (!centralizedParents &&
            ((numParents >= numTerminals && numParents % numTerminals != 0) ||
             (numParents < numTerminals && numTerminals % numParents != 0))) {
        panic("[mem_interconnect] %s: cannot assign %u parents to %u terminals, they are non-divisible.", name.c_str(), numParents, numTerminals);
    }
    assert(numParentsPerTerminal == 1 || numTerminalsPerParent == 1);
    if ((numChildren >= numTerminals && numChildren % numTerminals != 0) ||
            (numChildren < numTerminals && numTerminals % numChildren != 0)) {
        panic("[mem_interconnect] %s: cannot assign %u children to %u terminals, they are non-divisible.", name.c_str(), numChildren, numTerminals);
    }
    assert(numChildrenPerTerminal == 1 || numTerminalsPerChild == 1);

    if (pm->getTotal() != numParents) {
        panic("[mem_interconnect] %s: parent map must have %u total destination for parents, now %u.", name.c_str(), numParents, pm->getTotal());
    }

    assert(ra->getNumRouters() == routers.size());

    // Interfaces.
    // Each bottom interface represents a child to the parents.
    for (uint32_t i = 0; i < numChildren; i++) {
        std::stringstream ss;
        ss << name << "-bif-" << i;
        botIfs.push_back(new BottomInterface(this, ss.str().c_str()));
    }
    // Single top interface to the children.
    topIf = new TopInterface(this, name + "-tif");

    needsCSim = false;
    for (auto& r : routers) needsCSim |= r->needsCSim();
}

void MemInterconnect::setParents(const g_vector<MemObject*>& _parents) {
    if (!parents.empty()) {
        // Already set, ensure parents are the same.
        // Operator == for vector compares size first and then does element-wise comparison.
        assert_msg(parents == _parents, "[mem_interconnect] %s: different parents assigned.", name.c_str());
        return;
    }
    // First time, set.
    uint32_t numParents = numParentsPerTerminal * numTerminals / numTerminalsPerParent;
    if (!centralizedParents && _parents.size() != numParents)
        panic("[mem_interconnect] %s: expect %u parents, got %lu.", name.c_str(), numParents, _parents.size());
    parents.insert(parents.end(), _parents.begin(), _parents.end());
    // Print.
    std::string parentsName = parents.front()->getName();
    if (numParents > 1) parentsName += std::string("..") + parents.back()->getName();
    info("[mem_interconnect] %s: parents are %s (%u), %s.", name.c_str(), parentsName.c_str(), numParents, centralizedParents ? "centralized" : "distributed");
}

void MemInterconnect::setChildren(const g_vector<BaseCache*>& _children) {
    // Only set once.
    assert(children.empty());
    uint32_t numChildren = numChildrenPerTerminal * numTerminals / numTerminalsPerChild;
    if (_children.size() != numChildren)
        panic("[mem_interconnect] %s: expect %u children, got %lu.", name.c_str(), numChildren, _children.size());
    children.insert(children.end(), _children.begin(), _children.end());
    // Print.
    std::string childrenName = children.front()->getName();
    if (numChildren > 1) childrenName += std::string("..") + children.back()->getName();
    info("[mem_interconnect] %s: children are %s (%u).", name.c_str(), childrenName.c_str(), numChildren);
}

uint64_t MemInterconnect::processAccess(const MemReq& req) {
    uint64_t respCycle = req.cycle;
    const auto srcCoreId = req.srcId;
    const auto accType = req.type;

    // Child and parent.
    const uint32_t childId = req.childId;
    const uint32_t parentId = pm->preAccess(req.lineAddr, childId, req);

    assert(parentId < parents.size());
    assert(childId < children.size());

    // Terminals.
    uint32_t localId = getChildTerminalId(childId);
    uint32_t remoteId = getParentTerminalId(parentId);

    // Packet size.
    const uint64_t dataSize = 1 << lineBits;
    uint64_t sizeTo = 0;
    uint64_t sizeBack = 0;
    switch (accType) {
        case PUTX:
            // Writeback: send data and get acknowledge.
            sizeTo = ccHeaderSize + dataSize;
            sizeBack = ccHeaderSize;
            break;
        case GETS:
            // Read: send request and get data.
            sizeTo = ccHeaderSize;
            sizeBack = ccHeaderSize + dataSize;
            break;
        case PUTS:
        case GETX:
            // Clean evict or upgrade: send request and get permission, no data.
            sizeTo = ccHeaderSize;
            sizeBack = ccHeaderSize;
            break;

        default: panic("?!");
    }

    // Create the start event.
    auto evRec = zinfo->eventRecorders[srcCoreId];
    if (evRec && needsCSim) {
        TimingRecord tr;
        tr.clear();
        tr.addr = req.lineAddr;
        tr.type = req.type;
        auto startEv = new (evRec) DelayEvent(0);
        startEv->setMinStartCycle(respCycle);
        tr.startEvent = tr.endEvent = startEv;
        tr.reqCycle = tr.respCycle = respCycle;
        evRec->pushRecord(tr);
    }

    pm->postAccess(req.lineAddr, childId, req);

    // Travel through the interconnect from local to remote.
    respCycle = travel(localId, remoteId, sizeTo, respCycle, srcCoreId);

    // access() should be called with empty event recorder, so we pop the record and merge later.
    TimingRecord toRec;
    toRec.clear();
    if (evRec && evRec->hasRecord()) {
        toRec = evRec->popRecord();
    }

    // Access parent level.
    MemReq memReq = req;
    memReq.cycle = respCycle;
    respCycle = parents[parentId]->access(memReq);

    // Merge access record with to-trip record.
    if (toRec.isValid()) {
        assert(evRec);
        if (evRec->hasRecord()) {
            auto tr = evRec->popRecord();
            // Concatenate.
            assert(toRec.respCycle <= tr.reqCycle);
            auto dEv = new (evRec) DelayEvent(tr.reqCycle - toRec.respCycle);
            dEv->setMinStartCycle(toRec.respCycle);
            toRec.endEvent->addChild(dEv, evRec)->addChild(tr.startEvent, evRec);
            tr.startEvent = toRec.startEvent;
            tr.reqCycle = toRec.reqCycle;
            if (IsPut(tr.type)) {
                // PUTs are off the critical path. The end event should be the interconnect event.
                tr.respCycle = toRec.respCycle;
                tr.endEvent = toRec.endEvent;
            }
            tr.type = toRec.type;
            evRec->pushRecord(tr);
        } else {
            evRec->pushRecord(toRec);
        }
    }

    // Travel through the interconnect from remote to local.
    respCycle = travel(remoteId, localId, sizeBack, respCycle, srcCoreId);

    return respCycle;
}

uint64_t MemInterconnect::processInval(const InvReq& req, uint32_t childId) {
    uint64_t respCycle = req.cycle;
    const auto invType = req.type;

    // Child and parent.
    uint32_t parentId = pm->preInvalidate(req.lineAddr, childId, req);
    assert(parentId < parents.size());
    assert(childId < children.size());

    // Terminals.
    uint32_t localId = getParentTerminalId(parentId);
    uint32_t remoteId = getChildTerminalId(childId);

    // Packet size.
    const uint64_t dataSize = 1 << lineBits;
    uint64_t sizeTo = 0;
    uint64_t sizeBack = 0;
    switch (invType) {
        case FWD:
            // Writeback: send data and get acknowledge.
            sizeTo = ccHeaderSize + dataSize;
            sizeBack = ccHeaderSize;
            break;
        case INV:
        case INVX:
            // Invalidate and downgrade: send request and get permission, no data.
            sizeTo = ccHeaderSize;
            sizeBack = ccHeaderSize;
            break;

        default: panic("?!");
    }

    if (ignoreInvLatency) {
        respCycle = children[childId]->invalidate(req);
        return respCycle;
    }

    // FIXME(mgao): for now, zsim assumes that cache invalidation does not create any event.
    // So we pass a special -1 for the src core id to avoid creating events in routers.

    // Travel through the interconnect from local to remote.
    respCycle = travel(localId, remoteId, sizeTo, respCycle, -1);

    // Invalidate child.
    InvReq invReq = req;
    invReq.cycle = respCycle;
    respCycle = children[childId]->invalidate(invReq);

    pm->postInvalidate(req.lineAddr, childId, req);

    // Travel through the interconnect from remote to local.
    respCycle = travel(remoteId, localId, sizeBack, respCycle, -1);

    return respCycle;
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

