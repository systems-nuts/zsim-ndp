#include "mem_interconnect_interface.h"
#include <sstream>
#include "address_map.h"
#include "mem_interconnect.h"

MemInterconnectInterface::MemInterconnectInterface(MemInterconnect* _interconnect, uint32_t _index, AddressMap* _am,
        bool _centralizedParents, bool _ignoreInvLatency)
    : interconnect(_interconnect), index(_index), am(_am), centralizedParents(_centralizedParents), ignoreInvLatency(_ignoreInvLatency)
{
    // Lazily initialize after all children and parents are connected.
    numTerminals = 0;
    numParents = 0;
    numChildren = 0;
}

BaseCache* MemInterconnectInterface::getEndpoint(BaseCache* child, const g_string& name) {
    if (numChildren != 0)
        panic("[mem_interconnect] %s interface %u: can only initialize endpoints before connecting.", interconnect->getName(), index);

    auto e = new Endpoint(child, this, name);
    endpoints.push_back(e);
    return e;
}

uint64_t MemInterconnectInterface::accessParent(MemReq& req, uint32_t groupId) {
    uint64_t respCycle = req.cycle;

    // Determine child and parent.
    const uint32_t childId = req.childId;
    const uint32_t parentId = groups[groupId].map->getParentIdInAccess(req.lineAddr, childId, req);

    // Travel through the interconnect.
    respCycle = accReqTravel(req, respCycle, groupId, parentId, childId);

    // Access.
    MemReq req2 = req;
    req2.cycle = respCycle;
    respCycle = groups[groupId].parents[parentId]->access(req2);

    // Travel through the interconnect.
    respCycle = accRespTravel(req, respCycle, groupId, parentId, childId);

    return respCycle;
}

uint64_t MemInterconnectInterface::invalidateChild(const InvReq& req, uint32_t groupId, BaseCache* child, uint32_t childId) {
    uint64_t respCycle = req.cycle;

    // Determine child and parent.
    const uint32_t parentId = groups[groupId].map->getParentIdInInvalidate(req.lineAddr, childId, req);

    // Travel through the interconnect.
    respCycle = invReqTravel(req, respCycle, groupId, parentId, childId);

    // Invalidate.
    InvReq req2 = req;
    req2.cycle = respCycle;
    respCycle = child->invalidate(req2);

    // Travel through the interconnect.
    respCycle = invRespTravel(req, respCycle, groupId, parentId, childId);

    return respCycle;
}

uint64_t MemInterconnectInterface::accReqTravel(const MemReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId) {
    // Child -> parent.
    const uint32_t srcId = getChildTerminalId(groupId, childId);
    const uint32_t dstId = getParentTerminalId(groupId, parentId);
    return interconnect->accessRequest(req, cycle, srcId, dstId);
}

uint64_t MemInterconnectInterface::accRespTravel(const MemReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId) {
    // Parent -> child.
    const uint32_t srcId = getParentTerminalId(groupId, parentId);
    const uint32_t dstId = getChildTerminalId(groupId, childId);
    return interconnect->accessResponse(req, cycle, srcId, dstId);
}

uint64_t MemInterconnectInterface::invReqTravel(const InvReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId) {
    if (ignoreInvLatency) return cycle;

    // Parent -> child.
    const uint32_t srcId = getParentTerminalId(groupId, parentId);
    const uint32_t dstId = getChildTerminalId(groupId, childId);
    return interconnect->invalidateRequest(req, cycle, srcId, dstId);
}

uint64_t MemInterconnectInterface::invRespTravel(const InvReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId) {
    if (ignoreInvLatency) return cycle;

    // Child -> parent.
    const uint32_t srcId = getChildTerminalId(groupId, childId);
    const uint32_t dstId = getParentTerminalId(groupId, parentId);
    return interconnect->invalidateResponse(req, cycle, srcId, dstId);
}

uint32_t MemInterconnectInterface::getParentGroupId(const g_vector<MemObject*>& parents) {
    if (numTerminals != 0)
        panic("[mem_interconnect] %s interface %u: cannot connect to more children.", interconnect->getName(), index);

    // Get or make the parent group.
    uint32_t groupId = 0;
    while (groupId < groups.size()) {
        if (groups[groupId].parents == parents) break;
        groupId++;
    }
    if (groupId == groups.size()) {
        // A new group of parents.
        if (!groups.empty() && parents.size() != groups.front().parents.size()) {
            panic("[mem_interconnect] %s interface %u: all groups must have the same number of parents; expect %lu, but group %u has %lu.",
                    interconnect->getName(), index, groups.front().parents.size(), groupId, parents.size());
        }
        groups.emplace_back();
        groups.back().parents = parents;
        groups.back().map = new CoherentParentMap(am);
    }

    numChildren++;
    uint32_t totalChildren = endpoints.size();
    if (numChildren == totalChildren) {
        // Initialize everything.

        uint32_t numGroups = groups.size();

        // Number of terminals.
        uint32_t totalTerminals = interconnect->getNumTerminals();
        if (totalTerminals % numGroups != 0) {
            panic("[mem_interconnect] %s interface %u: total %u terminals cannot be partitioned into %u groups.",
                    interconnect->getName(), index, totalTerminals, numGroups);
        }
        numTerminals = totalTerminals / numGroups;

        // Number of parents.
        numParents = groups.empty() ? 0 : groups.front().parents.size();
        if (am->getTotal() != numParents) {
            panic("[mem_interconnect] %s interface %u: address map supports %u parents but expect %u.",
                    interconnect->getName(), index, am->getTotal(), numParents);
        }
        if (!centralizedParents && numParents % numTerminals != 0 && numTerminals % numParents != 0) {
            panic("[mem_interconnect] %s interface %u: %u parents are incompatible with %u terminals.",
                    interconnect->getName(), index, numParents, numTerminals);
        }

        // Number of children.
        if (totalChildren % numGroups != 0) {
            panic("[mem_interconnect] %s interface %u: total %u children cannot be partitioned into %u groups.",
                    interconnect->getName(), index, totalChildren, numGroups);
        }
        numChildren = totalChildren / numGroups;
        if (numChildren % numTerminals != 0 && numTerminals % numChildren != 0) {
            panic("[mem_interconnect] %s interface %u: %u children are incompatible with %u terminals.",
                    interconnect->getName(), index, numChildren, numTerminals);
        }

        info("[mem_interconnect] %s interface %u: %u groups, each has %u parents and %u children assigned to %u terminals.",
                interconnect->getName(), index, numGroups, numParents, numChildren, numTerminals);
    }

    return groupId;
}

MemInterconnectInterface::Endpoint* MemInterconnectInterface::getChildEndpoint(BaseCache* child) {
    for (auto e : endpoints) if (e->child == child) return e;
    panic("[mem_interconnect] %s interface %u: child %s does not have corresponding endpoint.", interconnect->getName(), index, child->getName());
}


void MemInterconnectInterface::Endpoint::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    if (network != nullptr)
        panic("[mem_interconnect] %s interface %u: cannot specify network with interconnect.", interface->interconnect->getName(), interface->index);
    for (auto c : children) endpointsOfChildren.push_back(interface->getChildEndpoint(c));
}

void MemInterconnectInterface::Endpoint::setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
    if (network != nullptr)
        panic("[mem_interconnect] %s interface %u: cannot specify network with interconnect.", interface->interconnect->getName(), interface->index);
    childId = _childId;
    groupId = interface->getParentGroupId(parents);
}

uint64_t MemInterconnectInterface::Endpoint::access(MemReq& req) {
    // Update child Id.
    // This access comes from a child of this endpoint, whose child Id is w.r.t. the endpoint.
    // We need to change it to be the child Id w.r.t. the parent, which is the child Id of the
    // corresponding endpoint of the child.
    assert(req.childId < endpointsOfChildren.size());
    req.childId = endpointsOfChildren[req.childId]->childId;
    return interface->accessParent(req, groupId);
}

uint64_t MemInterconnectInterface::Endpoint::invalidate(const InvReq& req) {
    return interface->invalidateChild(req, groupId, child, childId);
}

