#ifndef ADDRESS_MAP_H_
#define ADDRESS_MAP_H_

#include "constants.h"
#include "g_std/g_unordered_map.h"
#include "locks.h"
#include "memory_hierarchy.h"

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

        lock_t mapLock;

        // For each child, mapping from its current line to the original parent ID. The parent ID may be different from
        // the current address mapping.
        g_unordered_map<Address, uint32_t> childLineParentMap[MAX_CACHE_CHILDREN];

    public:
        explicit CoherentParentMap(AddressMap* _am) : am(_am) {
            futex_init(&mapLock);
        }

        uint32_t getTotal() const { return am->getTotal(); }

        /* We need to separate the pre and post actions to manage the mapping; specifically, the removal must be
         * postponed until we finish the access/invalidate.
         *
         * This is to avoid races between PUTS/X and INV (see CheckForMESIRace() in coherence_ctrls.h). In such a case,
         * we need to ensure both see the same parent ID. If we were to remove the mapping immediately after the lookup
         * of the first removal, the other removal would not locate the current parent ID.
         */

        inline uint32_t preAccess(Address lineAddr, uint32_t childId, const MemReq& req) {
            return getParentId(lineAddr, childId, req.type == GETS || req.type == GETX);
        }

        inline uint32_t preInvalidate(Address lineAddr, uint32_t childId, const InvReq& req) {
            return getParentId(lineAddr, childId, false);
        }

        inline void postAccess(Address lineAddr, uint32_t childId, const MemReq& req) {
            if ((req.type == PUTS || req.type == PUTX) && *req.state == I) removeParentId(lineAddr, childId);
        }

        inline void postInvalidate(Address lineAddr, uint32_t childId, const InvReq& req) {
            if (req.type == INV) removeParentId(lineAddr, childId);
        }

        uint32_t getParentId(Address lineAddr, uint32_t childId, bool shouldAdd) {
            if (!am->isDynamic()) return am->getMap(lineAddr);

            auto& lineParentMap = childLineParentMap[childId];
            uint32_t parentId = -1u;

            futex_lock(&mapLock);

            // Look up in the current lines.
            auto lIt = lineParentMap.find(lineAddr);
            if (lIt != lineParentMap.end()) {
                // Use the original parent.
                parentId = lIt->second;
            } else {
                // Must look up after holding the lock, to avoid race.
                parentId = am->getMap(lineAddr);
                if (shouldAdd) {
                    lineParentMap[lineAddr] = parentId;
                }
            }

            futex_unlock(&mapLock);
            return parentId;
        }

        void removeParentId(Address lineAddr, uint32_t childId) {
            if (!am->isDynamic()) return;

            auto& lineParentMap = childLineParentMap[childId];

            futex_lock(&mapLock);

            lineParentMap.erase(lineAddr);

            futex_unlock(&mapLock);
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

#endif  // ADDRESS_MAP_H_

