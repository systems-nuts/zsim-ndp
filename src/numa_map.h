#ifndef NUMA_MAP_H_
#define NUMA_MAP_H_

#include "bithacks.h"
#include "galloc.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "zsim.h"  // for lineBits

class PageMap;

class NUMAMap : public GlobAlloc {
    public:
        static constexpr uint32_t INVALID_NODE = -1u;

    public:
        NUMAMap(const char* _patchRoot, const uint32_t numCores);

        uint32_t getMaxNode() const {
            return maxNode;
        }

        uint32_t getNodeOfCore(const uint32_t cid) const {
            return coreNodeMap[cid];
        }

        uint32_t getNodeOfLineAddr(const Address lineAddr) {
            return getNodeOfPage(lineAddr >> (pageBits - lineBits));
        }

        uint32_t getNodeOfPage(const Address pageAddr);

        inline Address getPageAddress(const Address addr) {
            // NOTE: this must be equivalent to vAddr -> pLineAddr logic in filter_cache.h.
            return (addr >> 12) | (procMask >> (12 - lineBits));
        }

        // Add given pages to NUMA node. Return the pages that already exist and thus are ignored.
        size_t addPagesToNode(const Address pageAddr, const size_t pageCount, const uint32_t node);
        // Remove given pages from NUMA map.
        void removePages(const Address pageAddr, const size_t pageCount);

        // Use glob mem.
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;

    private:
        // Max node, assuming nodes are continuous.
        uint32_t maxNode;

        /* NUMA core map. */

        // Patched root path to provide the NUMA node map info.
        const g_string patchRoot;
        // Core-to-node map.
        g_vector<uint32_t> coreNodeMap;

        /* NUMA memory map. */

        // log2 of page size.
        uint32_t pageBits;
        // Page-to-node map.
        PageMap* pageNodeMap;

    private:
        // Parse the character string found in /sys/devices/system/node/nodeN/cpumap
        // to initialize core-to-node map.
        // Implemented after lib numactl-2.0.11 libnuma.c:numa_parse_bitmap_v2().
        int parseBitmap(const uint32_t node);
};

#endif  // NUMA_MAP_H_

