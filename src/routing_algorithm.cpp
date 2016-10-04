#include "routing_algorithm.h"
#include <algorithm>
#include "bithacks.h"

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

TreeRoutingAlgorithm::TreeRoutingAlgorithm(const g_vector<uint32_t>& levelSizes, bool onlyLeafTerminals) {
    if (levelSizes.empty()) panic("TreeRoutingAlgorithm: an empty tree!?");

    rootLevel = levelSizes.size() - 1;
    rootNumRouters = levelSizes[rootLevel];
    maxFanout = 1;

    std::vector<uint32_t> levelFanouts;
    for (uint32_t level = 0; level < rootLevel; level++) {
        if (levelSizes[level] < levelSizes[level + 1])
            panic("TreeRoutingAlgorithm: levels are from leaf to root, size should not increase at level %u", level + 1);
        if (levelSizes[level] % levelSizes[level + 1] != 0)
            panic("TreeRoutingAlgorithm: level %u fanout is not an integer, %u / %u", level, levelSizes[level], levelSizes[level + 1]);
        uint32_t f = levelSizes[level] / levelSizes[level + 1];
        if (f < 1) panic("TreeRoutingAlgorithm: fanout must be positive!");
        maxFanout = MAX(maxFanout, f);
        levelFanouts.push_back(f);
    }

    std::vector<uint32_t> levelIdOffsets;
    levelIdOffsets.push_back(0);
    for (auto n : levelSizes) levelIdOffsets.push_back(levelIdOffsets.back() + n);

    for (uint32_t level = 0; level <= rootLevel; level++) {
        assert(tuples.size() == levelIdOffsets[level]);
        for (uint32_t i = 0; i < levelSizes[level]; i++) {
            uint32_t parentId = level == rootLevel ? -1u : levelIdOffsets[level + 1] + i / levelFanouts[level];
            uint32_t firstChildId = level == 0 ? -1u : levelIdOffsets[level - 1] + i * levelFanouts[level - 1];
            tuples.emplace_back(level, parentId, firstChildId);
        }
    }
    assert(tuples.size() == levelIdOffsets.back());

    numRouters = levelIdOffsets.back();
    numTerminals = onlyLeafTerminals ? levelSizes[0] : levelIdOffsets.back();
}

void TreeRoutingAlgorithm::nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
    uint32_t curAncId = currentId;
    uint32_t dstAncId = destinationId;
    uint32_t ancLevel = uptrace(curAncId, dstAncId);
    if (curAncId != currentId) {
        // Going up.
        *nextId = tuples[currentId].parentId;
        *portId = PortUp;
    } else if (curAncId != dstAncId) {
        // Going horizontally at the top level.
        assert(ancLevel == rootLevel);
        *nextId = dstAncId;
        *portId = portHorizontal(dstAncId - (numRouters - rootNumRouters));
    } else {
        // Going down.
        assert(destinationId <= dstAncId);
        // Find the child to route to next, which must be an ancestor of the destination.
        uint32_t id = destinationId;
        while (tuples[id].level < ancLevel - 1) {
            id = tuples[id].parentId;
        }
        assert(tuples[id].parentId == dstAncId);
        *nextId = id;
        *portId = portDown(id - tuples[dstAncId].firstChildId);
    }

    DEBUG("TreeRoutingAlgorithm: %u -> %u: %u, ancestor %u - %u at level %u", currentId, destinationId,
            *nextId, curAncId, dstAncId, ancLevel);
}

uint32_t TreeRoutingAlgorithm::uptrace(uint32_t& id1, uint32_t& id2) const {
    // Ensure p1 points to the larger (closer to root) one.
    uint32_t* p1 = &id1;
    uint32_t* p2 = &id2;
    if (*p1 < *p2) std::swap(p1, p2);

    uint32_t level = tuples[*p1].level;
    while (tuples[*p2].level != level) {
        *p2 = tuples[*p2].parentId;
    }
    // Now both are at the same level.

    while (id1 != id2 && level != rootLevel) {
        id1 = tuples[id1].parentId;
        id2 = tuples[id2].parentId;
        level++;
    }
    assert(level == tuples[id1].level && tuples[id1].level == tuples[id2].level);

    return level;
}

