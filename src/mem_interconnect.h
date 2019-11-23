#ifndef MEM_INTERCONNECT_H_
#define MEM_INTERCONNECT_H_

#include <sstream>
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "stats.h"

class CoherentParentMap;
class MemRouter;
class RoutingAlgorithm;

/**
 * A MemInterconnect instance is between a single (maybe multi-bank) parent cache and its child caches.
 * Inter-parent-cache interconnect is not needed.
 */
class MemInterconnect : public GlobAlloc {
    public:
        /**
         * Interface of the interconnect to the neighbor (top/bottom) cache levels.
         *
         * There is a single TopInterface accepting the accesses from all children, and multiple BottomInterfaces
         * accepting the invalidations from the parents. Each BottomInterface corresponds to one child, and the
         * invalidations from the parents to that child are forwarded to it. This approach matches the childId between
         * access() and invalidate().
         */

        class TopInterface : public BaseCache {
            private:
                MemInterconnect* itcn;
                const g_string name;

            public:
                TopInterface(MemInterconnect* _itcn, const g_string& _name) : itcn(_itcn), name(_name) {}

                const char* getName() { return name.c_str(); }
                void initStats(AggregateStat* parentStat) {}

                void setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
                    // No parents, do nothing.
                }
                void setChildren(const g_vector<BaseCache*>& children, Network* network) {
                    // Children are the (all-flattened) children for the interconnect.
                    if (network != nullptr) warn("[mem_interconnect] Network latency is ignored.");
                    // The single TopInterface sets the children once for the interconnect.
                    itcn->setChildren(children);
                }

                uint64_t access(MemReq& req) {
                    return itcn->processAccess(req);
                }

                uint64_t invalidate(const InvReq& req) {
                    panic("%s: Interconnect top interface should never receive INV!", getName());
                }
        };

        class BottomInterface : public BaseCache {
            private:
                MemInterconnect* itcn;
                uint32_t childId;
                const g_string name;

            public:
                BottomInterface(MemInterconnect* _itcn, const g_string& _name) : itcn(_itcn), childId(-1), name(_name) {}

                const char* getName() { return name.c_str(); }
                void initStats(AggregateStat* parentStat) {}

                void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
                    // Parents are the parents for the interconnect.
                    if (network != nullptr) warn("[mem_interconnect] Network latency is ignored.");
                    childId = _childId;
                    // Must ensure parents are the same for all BottomInterfaces, and only set once.
                    itcn->setParents(parents);
                }
                void setChildren(const g_vector<BaseCache*>& children, Network* network) {
                    // No children, do nothing.
                }

                uint64_t access(MemReq& req) {
                    panic("%s: Interconnect bottom interface should never receive ACC!", getName());
                }

                uint64_t invalidate(const InvReq& req) {
                    return itcn->processInval(req, childId);
                }
        };

    private:
        /**
         * Methods for Interface class.
         */

        // Set parents.
        void setParents(const g_vector<MemObject*>& _parents);

        // Set children.
        void setChildren(const g_vector<BaseCache*>& _children);

        // Process an access request entering the interconnect.
        uint64_t processAccess(const MemReq& req);

        // Process an invalidation request sent to a child through the interconnect.
        uint64_t processInval(const InvReq& req, uint32_t childId);

    public:
        MemInterconnect(RoutingAlgorithm* _ra, CoherentParentMap* _pm, const g_vector<MemRouter*>& _routers,
                uint32_t numParents, uint32_t numChildren, bool _centralizedParents,
                uint32_t _ccHeaderSize, bool _ignoreInvLatency, const g_string& _name);

        const char* getName() { return name.c_str(); }

        g_vector<BaseCache*> getBottomInterface() const {
            g_vector<BaseCache*> bifs;
            bifs.insert(bifs.end(), botIfs.begin(), botIfs.end());
            return bifs;
        }

        g_vector<BaseCache*> getTopInterface() const {
            return g_vector<BaseCache*>(1, topIf);
        }

    private:
        RoutingAlgorithm* ra;
        CoherentParentMap* pm;
        g_vector<MemRouter*> routers;
        const uint32_t numTerminals;

        const bool centralizedParents;
        const uint32_t ccHeaderSize;
        const bool ignoreInvLatency;

        g_vector<MemObject*> parents;
        g_vector<BaseCache*> children;
        uint32_t numParentsPerTerminal;
        uint32_t numTerminalsPerParent;
        uint32_t numChildrenPerTerminal;
        uint32_t numTerminalsPerChild;
        g_vector<BottomInterface*> botIfs;
        TopInterface* topIf;

        bool needsCSim;

        const g_string name;

    private:
        // Travel a packet through the routers in the interconnect.
        uint64_t travel(uint32_t srcId, uint32_t dstId, size_t size, uint64_t cycle, uint32_t srcCoreId);

        inline uint32_t getParentTerminalId(uint32_t parentId) {
            if (centralizedParents) {
                return numTerminals / 2;
            }
            // If num parents per terminal is not 1, uniformly distribute parents to terminal.
            // If num terminals per parent is not 1, put each parent at the central terminal of its partition.
            return (parentId / numParentsPerTerminal) * numTerminalsPerParent + numTerminalsPerParent / 2;
        }

        inline uint32_t getChildTerminalId(uint32_t childId) {
            // If num children per terminal is not 1, uniformly distribute children to terminal.
            // If num terminals per child is not 1, put each child at the central terminal of its partition.
            return (childId / numChildrenPerTerminal) * numTerminalsPerChild + numTerminalsPerChild / 2;
        }

    public:
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

#endif  // MEM_INTERCONNECT_H_

