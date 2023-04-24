#ifndef MEM_INTERCONNECT_INTERFACE_H_
#define MEM_INTERCONNECT_INTERFACE_H_

#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"

class AddressMap;
class CoherentParentMap;
class MemInterconnect;

/**
 * An interface of an interconnect to neighboring parent/child memory hierarchy levels.
 *
 * Multiple parent/child levels can use different interfaces to the same underlying interconnect.
 */
class MemInterconnectInterface : public GlobAlloc {
    public:
        MemInterconnectInterface(MemInterconnect* _interconnect, uint32_t _index, AddressMap* _am,
                bool _centralizedParents, bool _ignoreInvLatency);

        // Construct and return the endpoint associated with the given child cache.
        BaseCache* getEndpoint(BaseCache* child, const g_string& name);

        /**
         * The interconnect interface has a set of endpoints, which are cache-like objects, and are inserted into the
         * memory hierarchy, acting as the parents of the child caches of the interconnect and the children of the
         * parent caches of the interconnect.
         *
         * Each endpoint corresponds to one child, and the endpoints have the same organization as the children. So when
         * the endpoints are presented to the parents, they get the same child Ids as the children originally should
         * have got.
         *
         * The child makes accesses to one of the endpoints (may or may not be the corresponding one due to uncontrolled
         * address mapping), which, after updating the child Id, turns to the interface. The interface then directs the
         * accesses to the correct parent based on address mapping, and handles interconnect traffic.
         *
         * The parent makes invalidates to an endpoint, which always corresponds to the target child. The endpoint also
         * turns to the interface, with the target child information. The interface figures out the parent from which
         * the invalidates come based on address mapping, and handles interconnect traffic.
         */

        class Endpoint : public BaseCache {
            protected:
                BaseCache* child;
                uint32_t childId;
                uint32_t groupId;

                MemInterconnectInterface* interface;

                // These are the corresponding endpoints of the children of this endpoint.
                // Used to figure out the actual child Id when accessing the parent.
                g_vector<Endpoint*> endpointsOfChildren;

                const g_string name;

                friend class MemInterconnectInterface;

            public:
                Endpoint(BaseCache* _child, MemInterconnectInterface* _interface, const g_string& _name)
                    : child(_child), interface(_interface), name(_name) {}

                const char* getName() { return name.c_str(); }

                void setChildren(const g_vector<BaseCache*>& children, Network* network);

                void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
                
                uint64_t forgeAccess(MemReq& req, uint32_t parentId) {
                    return this->interface->forgeAccessParent(req, groupId, parentId);
                }
                uint64_t access(MemReq& req);

                uint64_t invalidate(const InvReq& req);
        };
    protected: 
        uint64_t forgeAccessParent(MemReq& req, uint32_t groupId, uint32_t parentId);
        uint64_t accessParent(MemReq& req, uint32_t groupId);

        uint64_t invalidateChild(const InvReq& req, uint32_t groupId, BaseCache* child, uint32_t childId);

    protected:

        /* Interface to the interconnect. Can be overwritten. */

        virtual uint64_t accReqTravel(const MemReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId);

        virtual uint64_t accRespTravel(const MemReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId);

        virtual uint64_t invReqTravel(const InvReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId);

        virtual uint64_t invRespTravel(const InvReq& req, uint64_t cycle, uint32_t groupId, uint32_t parentId, uint32_t childId);

    protected:

        /* Parents/children. */

        uint32_t getParentGroupId(const g_vector<MemObject*>& parents);

        Endpoint* getChildEndpoint(BaseCache* child);

        /* Mapping (groupId, parentId/childId) to terminalId. */

        inline uint32_t getParentTerminalId(uint32_t groupId, uint32_t parentId) {
            assert(parentId < numParents);
            if (centralizedParents) {
                return getTerminalId(groupId, 0, 1, numTerminals);
            }
            return getTerminalId(groupId, parentId, numParents, numTerminals);
        }

        inline uint32_t getChildTerminalId(uint32_t groupId, uint32_t childId) {
            assert(childId < numChildren);
            return getTerminalId(groupId, childId, numChildren, numTerminals);
        }

        // Common (groupId, parentId/childId) -> terminalId function. Works with different numbers of parents/children and terminals.
        inline static uint32_t getTerminalId(uint32_t groupId, uint32_t objectId, uint32_t numObjects, uint32_t numTerminals) {
            // If objects per terminal > 1, uniformly distribute objects to terminals.
            if (numObjects >= numTerminals) {
                uint32_t numObjectsPerTerminal = numObjects / numTerminals;
                return groupId * numTerminals + objectId / numObjectsPerTerminal;
            }
            // If terminals per object > 1, put each object at the central terminal of its partition.
            uint32_t numTerminalsPerObject = numTerminals / numObjects;
            return groupId * numTerminals + objectId * numTerminalsPerObject + numTerminalsPerObject / 2;
        }

        inline bool isRemote(uint32_t groupId, uint32_t parentId, uint32_t childId) {
            return getParentTerminalId(groupId, parentId) != getChildTerminalId(groupId, childId);
        }

    protected:
        MemInterconnect* interconnect;
        const uint32_t index;

        AddressMap* am;

        // All banks of each parent cache and its children form a group.
        struct GroupInfo {
            g_vector<MemObject*> parents;
            CoherentParentMap* map;
        };
        g_vector<GroupInfo> groups;

        // All endpoints of this interface.
        g_vector<Endpoint*> endpoints;

        // Per-group numbers; uniform across groups for now but could be extended to be heterogeneous.
        uint32_t numTerminals;
        uint32_t numParents;
        uint32_t numChildren;

        const bool centralizedParents;
        const bool ignoreInvLatency;

    public:
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

#endif  // MEM_INTERCONNECT_INTERFACE_H_

