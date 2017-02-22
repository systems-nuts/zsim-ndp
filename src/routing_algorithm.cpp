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


HomoHierRoutingAlgorithm::HomoHierRoutingAlgorithm(const g_vector<RoutingAlgorithm*>& _levels)
    : levels(_levels)
{
    if (levels.size() <= 1) panic("HomoHierRoutingAlgorithm: found no more than 1 level!");

    // level -> group count.
    for (uint32_t level = 0; level < levels.size(); level++) {
        uint32_t numTerminals = levels[level]->getNumTerminals();
        for (auto& c : levelGroupCounts) c *= numTerminals;
        levelGroupCounts.push_back(1);
    }

    // level -> starting router Id.
    // router Id -> tuple.
    for (uint32_t level = 0; level < levels.size(); level++) {
        levelIdOffsets.push_back(tuples.size());

        uint32_t numTerminals = levels[level]->getNumTerminals();
        uint32_t numRouters = levels[level]->getNumRouters();

        for (uint32_t g = 0; g < levelGroupCounts[level]; g++) {
            for (uint32_t i = 0; i < numTerminals; i++) {
                tuples.emplace_back(level, g, i);
            }
        }
        for (uint32_t g = 0; g < levelGroupCounts[level]; g++) {
            for (uint32_t i = numTerminals; i < numRouters; i++) {
                tuples.emplace_back(level, g, i);
            }
        }
    }
    levelIdOffsets.push_back(tuples.size());
    for (uint32_t i = 0; i < tuples.size(); i++) assert(tuple2Id(tuples[i]) == i);

    // Number of leaf terminals.
    numTerminals = levelGroupCounts[0] * levels[0]->getNumTerminals();

    // Overall center router is the top ancestor.
    centerRouterId = tuple2Id(getAncestor(0, levels.size() - 1));

    // The maximum number of ports across routers of all levels.
    numPorts = 0;
    for (uint32_t level = 0; level < levels.size(); level++) {
        numPorts = MAX(numPorts, levels[level]->getNumPorts());
    }
    // Routers at each level need the normal number of ports, one more to the center router of the lower level,
    // and one more to the upper level (if it is the center router of this level).
    numPorts += 2;
    // The second last port goes to lower level, and the last port goes to upper level.
    portIdDownward = numPorts - 2;
    portIdUpward = numPorts - 1;

    info("HomoHierRoutingAlgorithm: %lu levels, number of leaf terminals %u", levels.size(), numTerminals);
    info("HomoHierRoutingAlgorithm: per level: number of network instances %s, start router id %s",
            Str(levelGroupCounts).c_str(), Str(levelIdOffsets).c_str());
}

void HomoHierRoutingAlgorithm::nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
    assert_msg(tuples[destinationId].level == 0, "HomoHierRoutingAlgorithm: only leaf level router can be destination.");

    // Level of the current router.
    uint32_t level = tuples[currentId].level;

    // Ancestor routers in the current level.
    auto curAnc = getAncestor(currentId, level);
    auto dstAnc = getAncestor(destinationId, level);
    auto nxtAnc(curAnc);

    if (curAnc.group == dstAnc.group) {
        if (curAnc.local == dstAnc.local) {
            // We are at the ancestor router of the destination, go downward.
            assert_msg(level != 0, "HomoHierRoutingAlgorithm: already arrived!?");
            movesDown(nxtAnc);
            *nextId = tuple2Id(nxtAnc);
            *portId = portIdDownward;
        } else {
            // We are in the same parent terminal, route within it.
            levels[level]->nextHop(curAnc.local, dstAnc.local, &nxtAnc.local, portId);
            *nextId = tuple2Id(nxtAnc);
        }
    } else {
        uint32_t center = levels[level]->getCenterRouterId();
        if (curAnc.local != center) {
            // We are in different parent terminals, go the the center in order to go upward.
            levels[level]->nextHop(curAnc.local, center, &nxtAnc.local, portId);
            *nextId = tuple2Id(nxtAnc);
        } else {
            // Already at center, go upward.
            assert_msg(level != levels.size() - 1, "HomoHierRoutingAlgorithm: top root must be a common ancestor.");
            movesUp(nxtAnc);
            *nextId = tuple2Id(nxtAnc);
            *portId = portIdUpward;
        }
    }

    DEBUG("HomoHierRoutingAlgorithm: %u -> %u: %s - %s - %s", currentId, destinationId,
            curAnc.str().c_str(), nxtAnc.str().c_str(), dstAnc.str().c_str());
}

uint32_t HomoHierRoutingAlgorithm::tuple2Id(const RouterTuple& tuple) const {
    uint32_t numTerminals = levels[tuple.level]->getNumTerminals();
    uint32_t numRouters = levels[tuple.level]->getNumRouters();
    if (tuple.local < numTerminals) {
        // Terminal router.
        return levelIdOffsets[tuple.level] + tuple.group * numTerminals + tuple.local;
    }
    // Non-terminal router.
    return levelIdOffsets[tuple.level] + levelGroupCounts[tuple.level] * numTerminals
        + tuple.group * (numRouters - numTerminals) + (tuple.local - numTerminals);
}

void HomoHierRoutingAlgorithm::movesUp(RouterTuple& tuple) const {
    // Group Id reduces by the factor of the number of terminals of the upper level. Each group becomes a terminal.
    uint32_t reduction = levels[tuple.level + 1]->getNumTerminals();  // number of terminals of the upper level
    tuple.level++;
    assert_msg(tuple.level < levels.size(), "HomoHierRoutingAlgorithm: move up above root!?");
    uint32_t group = tuple.group;
    tuple.group = group / reduction;
    tuple.local = group % reduction;  // in each network, router Id == terminal Id
}

void HomoHierRoutingAlgorithm::movesDown(RouterTuple& tuple) const {
    // Group Id expands by the factor of the number of terminals of the upper level. Each terminal becomes a group.
    uint32_t expansion = levels[tuple.level]->getNumTerminals();
    assert_msg(tuple.level > 0, "HomoHierRoutingAlgorithm: move down below leaf!?");
    tuple.level--;
    tuple.group = tuple.group * expansion + tuple.local;
    tuple.local = levels[tuple.level]->getCenterRouterId();
}

HomoHierRoutingAlgorithm::RouterTuple HomoHierRoutingAlgorithm::getAncestor(uint32_t routerId, uint32_t level) const {
    RouterTuple tuple = tuples[routerId];
    assert_msg(tuple.level <= level, "HomoHierRoutingAlgorithm: ancestor level %u is below router %u.", level, routerId);
    while (tuple.level < level) movesUp(tuple);
    return tuple;
}

