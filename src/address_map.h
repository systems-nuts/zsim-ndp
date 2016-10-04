#ifndef ADDRESS_MAP_H_
#define ADDRESS_MAP_H_

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

