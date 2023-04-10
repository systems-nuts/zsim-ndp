#ifndef MEM_INTERCONNECT_H_
#define MEM_INTERCONNECT_H_

#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "stats.h"

class MemRouter;
class RoutingAlgorithm;

/**
 * An interconnect contains the topology and routers.
 *
 * Neighboring memory hierarchy levels interact with the interconnect through an interface.
 */
class MemInterconnect : public GlobAlloc {
    public:
        MemInterconnect(RoutingAlgorithm* _ra, const g_vector<MemRouter*>& _routers, uint32_t _ccHeaderSize, const g_string& _name);

        const char* getName() { return name.c_str(); }

        uint32_t getNumTerminals() const { return numTerminals; }

        uint64_t accessRequest(const MemReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId);

        uint64_t accessResponse(const MemReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId);

        uint64_t invalidateRequest(const InvReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId);

        uint64_t invalidateResponse(const InvReq& req, uint64_t cycle, uint32_t srcId, uint32_t dstId);

    private:
        RoutingAlgorithm* ra;
        g_vector<MemRouter*> routers;
        const uint32_t numTerminals;
        const uint32_t ccHeaderSize;

        bool needsCSim;

        const g_string name;

    private:
        // Travel a packet through the routers in the interconnect.
        uint64_t travel(uint32_t srcId, uint32_t dstId, size_t size, uint64_t cycle, bool piggyback, uint32_t srcCoreId);

    public:
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

#endif  // MEM_INTERCONNECT_H_

