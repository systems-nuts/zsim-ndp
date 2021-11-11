#ifndef ADDRESS_MAP_H_
#define ADDRESS_MAP_H_

#include <bitset>
#include "constants.h"
#include "g_std/g_unordered_map.h"
#include "locks.h"
#include "memory_hierarchy.h"
#include "numa_map.h"
#include "zsim.h"
// #include "log.h"

/**
 * Map an address to an integer-map, which can be parent cache bank, NUMA node, etc..
 *
 * FIXME(gaomy): should be merged with the getParentId() function in MESIBottomCC class.
 */
class AddressMap : public GlobAlloc {
    public:
        virtual uint32_t getTotal() const = 0;
        virtual uint32_t getMap(Address lineAddr) const = 0;
        virtual bool isDynamic() const { return false; }
};

/**
 * For dynamic address mapping: parent could change; take care of coherence.
 *
 * Assume the parent of a line is changed from p0 to p1. A child c has the line. For child c, the parent of the line
 * should stay as p0 until it is evicted, while any new access from other children use parent p1.
 *
 * This has minor impact on coherence. First, if p0 and p1 are not the last level, both will show as sharers in their
 * parent, so coherence is maintained. Second, migration typically happened when an address is remapped, so the original
 * child c should not access the address any more.
 */
class CoherentParentMap : public GlobAlloc {
    private:
        AddressMap* am;
        // Support dynamic address mapping and line migration between parents.
        lock_t mapLock;
        // Mapping from line to current parent ID and child sharer IDs.
        g_unordered_map<Address, std::pair<uint32_t, std::bitset<MAX_CACHE_CHILDREN>>> lineParentChildren;
        // Mapping from migrated line to original parent ID and its remaining child sharer IDs.
        g_unordered_map<Address, std::pair<uint32_t, std::bitset<MAX_CACHE_CHILDREN>>> migratedLineParentChildren;

    public:
        explicit CoherentParentMap(AddressMap* _am) : am(_am) {
            futex_init(&mapLock);
        }

        uint32_t getTotal() const { return am->getTotal(); }

        inline uint32_t getParentIdInAccess(Address lineAddr, uint32_t childId, const MemReq& req) {
            return getParentId(lineAddr, childId,
                    req.type == GETS || req.type == GETX,
                    req.type == PUTS || (req.type == PUTX && !req.is(MemReq::PUTX_KEEPEXCL)));
        }

        inline uint32_t getParentIdInInvalidate(Address lineAddr, uint32_t childId, const InvReq& req) {
            return getParentId(lineAddr, childId, false, req.type == INV);
        }

        uint32_t getParentId(Address lineAddr, uint32_t childId, bool shouldAdd, bool shouldRemove) {
            uint32_t parentId = am->getMap(lineAddr);
            if (!am->isDynamic()) return parentId;

            assert(!shouldAdd || !shouldRemove);

            futex_lock(&mapLock);

            // Look up in the migrated lines.
            bool isMigratedLine = false;
            auto migIt = migratedLineParentChildren.find(lineAddr);
            if (migIt != migratedLineParentChildren.end()) {
                auto& sharers = migIt->second.second;
                if (sharers[childId]) {
                    // Use the original parent.
                    parentId = migIt->second.first;
                    isMigratedLine = true;

                    // Do not add; keep using the original parent until removed.
                    shouldAdd = false;
                    if (shouldRemove) {
                        // The line has been handled for this child; remove.
                        sharers.set(childId, false);
                        if (sharers.none()) {
                            // The line has been handled for all children; forget it as migrated.
                            migratedLineParentChildren.erase(migIt);
                        }
                    }
                }
            }
            if (!isMigratedLine) {
                // Check if migrated.
                auto lineIt = lineParentChildren.find(lineAddr);
                if (lineIt != lineParentChildren.end() && lineIt->second.first != parentId) {
                    // Parent has changed. Add to migrated lines.
                    auto& sharers = lineIt->second.second;
                    if (sharers[childId]) {
                        if (shouldRemove) {
                            // Leave the requesting child to the actual access to handle.
                            sharers.set(childId, false);
                        }
                        parentId = lineIt->second.first;
                        // This sharer will keep using the original parent.
                        // If the child is not a sharer, then it will be added and use the new parent.
                        shouldAdd = false;
                    }
                    migratedLineParentChildren[lineAddr] = lineIt->second;
                    // Remove from current lines.
                    lineParentChildren.erase(lineIt);
                    shouldRemove = false;
                }

                // Update current lines.
                if (shouldAdd) {
                    lineParentChildren[lineAddr].first = parentId;
                    lineParentChildren[lineAddr].second.set(childId, true);
                }
                if (shouldRemove) {
                    if (lineIt != lineParentChildren.end()) {
                        auto& sharers = lineIt->second.second;
                        sharers.set(childId, false);
                        if (sharers.count() == 0) {
                            lineParentChildren.erase(lineIt);
                        }
                    }
                }
            }

            futex_unlock(&mapLock);
            return parentId;
        }
};

/**
 * Hash the address by splitting into four 16-bit chunks and XOR together.
 *
 * Same as the hash used in MESIBottomCC::getParentId().
 */
class XOR16bHashAddressMap : public AddressMap {
    private:
        const uint32_t total;

    public:
        explicit XOR16bHashAddressMap(uint32_t _total) : total(_total) {}

        uint32_t getTotal() const { return total; }

        uint32_t getMap(Address lineAddr) const {
            uint32_t res = 0;
            uint64_t tmp = lineAddr;
            for (uint32_t i = 0; i < 4; i++) {
                res ^= (uint32_t) ( ((uint64_t)0xffff) & tmp);
                tmp = tmp >> 16;
            }
            return res % total;
        }
};

/**
 * Static address interleaving across nodes.
 */
class StaticInterleavingAddressMap : public AddressMap {
    private:
        const Address chunkNumLines;
        const uint32_t total;

    public:
        StaticInterleavingAddressMap(Address _chunkNumLines, uint32_t _total)
            : chunkNumLines(_chunkNumLines), total(_total) {}

        uint32_t getTotal() const { return total; }

        uint32_t getMap(Address lineAddr) const {
            return (lineAddr / chunkNumLines) % total;
        }
};

/**
 * Map address according to NUMA.
 *
 * zinfo->numaMap must be valid. Otherwise assume a single node.
 */


class NUMAAddressMap : public AddressMap {
    private:
        const uint32_t total;
        const uint32_t nodes;
        XOR16bHashAddressMap* nodeMap;

    public:
        NUMAAddressMap(uint32_t _total)
            : total(_total), nodes(zinfo->numaMap ? zinfo->numaMap->getMaxNode() + 1 : 1) {
            if (total % nodes != 0)
                panic("NUMAAddressMap: total terminals (%u) must be a multiple of NUMA nodes (%u)", total, nodes);
            nodeMap = new XOR16bHashAddressMap(total / nodes);
        }

        ~NUMAAddressMap() { 
            delete nodeMap; 
        }

        uint32_t getTotal() const { 
            return total; 
        }

        uint32_t getMap(Address lineAddr) const {
            auto n = zinfo->numaMap->getNodeOfLineAddr(lineAddr);
            return n * total / nodes + nodeMap->getMap(lineAddr);
        }

        bool isDynamic() const override { return true; }
};




#endif  // ADDRESS_MAP_H_

