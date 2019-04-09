#ifndef CC_EXTS_H_
#define CC_EXTS_H_

#include "coherence_ctrls.h"
#include "event_recorder.h"
#include "g_std/g_string.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"
#include "locks.h"
#include "memory_hierarchy.h"
#include "network.h"
#include "zsim.h"

/**
 * MESI CC for a coherence directory hub with no actual data storage.
 *
 * The CC maintains a directory of sharers of cachelines, and uses this directory to maintain coherence among children.
 * It can be used as a dummy parent without actually introducing another cache level.
 *
 * On data access from a child, if children forwarding is enabled and any of the other children have already cached the
 * requested data, the closest child to the requesting child is asked to serve the data by forwarding. Otherwise the
 * next parent level serves the data. No data hit at this level.
 */
template<bool ChildrenForwarding = true>
class MESIDirectoryHubCC : public MESICC {
    protected:
        /* This class is a hacky way to record the info needed for parent call for each line, without internal
         * access to the base CC. Essentially it provides a callback point when accessing the parents.
         */
        class ParentPoint : public MemObject {
            public:
                ParentPoint(MESIDirectoryHubCC* _cc, MemObject* _parent, uint32_t _parentId)
                    : cc(_cc), parent(_parent), parentId(_parentId),
                      name(_parent->getName())  // use the same name as the parent so network can pair them
                {}

                uint64_t access(MemReq& req) {
                    // Access to the parent.
                    uint64_t respCycle = parent->access(req);

                    // Record line info mapping.
                    if (req.type == GETS || req.type == GETX) {
                        // A line is fetched, add.
                        cc->addLineInfo(req, parentId);
                    } else {
                        assert(req.type == PUTS || req.type == PUTX);
                        if (req.type != PUTX || !req.is(MemReq::PUTX_KEEPEXCL)) {
                            // A line is evicted, remove.
                            // This may race with invalidates and remove an already removed line.
                            cc->removeLineInfo(req.lineAddr);
                        }
                    }

                    // Record child lock.
                    if (likely(cc->bottomLock != nullptr)) {
                        assert(cc->bottomLock == req.childLock);
                    } else {
                        cc->bottomLock = req.childLock;
                    }

                    return respCycle;
                }

                const char* getName() { return name.c_str(); }

            private:
                MESIDirectoryHubCC* cc;
                MemObject* parent;
                uint32_t parentId;
                g_string name;
        };

        inline void addLineInfo(const MemReq& req, uint32_t parentId) {
            assert(req.type == GETS || req.type == GETX);
            if (lineInfoMap.count(req.lineAddr)) {
                // Already exists, must be consistent.
                assert(lineInfoMap.at(req.lineAddr).parentId == parentId);
                assert(lineInfoMap.at(req.lineAddr).state == req.state);
            } else {
                // Add as a new line.
                lineInfoMap[req.lineAddr] = {parentId, req.state};
            }
        }

        inline uint32_t removeLineInfo(Address lineAddr) {
            return lineInfoMap.erase(lineAddr);
        }

    protected:
        // Children.
        g_vector<BaseCache*> children;

        // Parents.
        g_vector<MemObject*> parents;
        g_vector<uint32_t> parentRTTs;
        uint32_t selfId;
        struct LineInfo {
            uint32_t parentId;
            MESIState* state;
        };
        g_unordered_map<Address, LineInfo> lineInfoMap;  // indexed by line address
        lock_t* bottomLock;

        Counter profGETSFwd, profGETXFwdIM, profGETXFwdSM;
        Counter profGETSRly, profGETXRlyIM, profGETXRlySM;
        Counter profGETRlyNextLevelLat, profGETRlyNetLat;

    public:
        MESIDirectoryHubCC(uint32_t _numLines, bool _nonInclusiveHack, g_string& _name)
            : MESICC(_numLines, _nonInclusiveHack, _name), selfId(-1u), bottomLock(nullptr) {}

        void setChildren(const g_vector<BaseCache*>& _children, Network* network) {
            children.assign(_children.begin(), _children.end());
            MESICC::setChildren(_children, network);
        }

        void setParents(uint32_t childId, const g_vector<MemObject*>& _parents, Network* network) {
            parents.assign(_parents.begin(), _parents.end());
            parentRTTs.resize(_parents.size());
            for (uint32_t p = 0; p < _parents.size(); p++) {
                parentRTTs[p] = network ? network->getRTT(name.c_str(), _parents[p]->getName()) : 0;
            }
            selfId = childId;

            // Insert callback points.
            g_vector<MemObject*> ppoints;
            for (uint32_t p = 0; p < _parents.size(); p++) {
                ppoints.push_back(new ParentPoint(this, _parents[p], p));
            }
            MESICC::setParents(childId, ppoints, network);
        }

        void initStats(AggregateStat* cacheStat) {
            profGETSFwd.init("fwdGETS", "GETS forwards");
            profGETXFwdIM.init("fwdGETXIM", "GETX I->M forwards");
            profGETXFwdSM.init("fwdGETXSM", "GETX S->M forwards (upgrade forwards)");
            profGETSRly.init("rlyGETS", "Relayed GETS fetches");
            profGETXRlyIM.init("rlyGETXIM", "Relayed GETX I->M fetches");
            profGETXRlySM.init("rlyGETXSM", "Relayed GETX S->M fetches (upgrade fetches)");
            profGETRlyNextLevelLat.init("latRlyGETnl", "Relayed GET request latency on next level");
            profGETRlyNetLat.init("latRlyGETnet", "Relayed GET request latency on network to next level");

            if (ChildrenForwarding) {
                cacheStat->append(&profGETSFwd);
                cacheStat->append(&profGETXFwdIM);
                cacheStat->append(&profGETXFwdSM);
            } else {
                cacheStat->append(&profGETSRly);
                cacheStat->append(&profGETXRlyIM);
                cacheStat->append(&profGETXRlySM);
            }

            // All hits on the directory are replaced as forwarding.
            auto dummyStat = new AggregateStat();
            dummyStat->init(name.c_str(), "Dummy stats");
            MESICC::initStats(dummyStat);
            for (uint32_t i = 0; i < dummyStat->curSize(); i++) {
                auto s = dummyStat->get(i);
                std::string name = s->name();
                if (name == "hGETX" || name == "hGETS") continue;
                cacheStat->append(s);
            }
            delete dummyStat;

            if (!ChildrenForwarding) {
                cacheStat->append(&profGETRlyNextLevelLat);
                cacheStat->append(&profGETRlyNetLat);
            }
        }

        uint64_t processAccess(const MemReq& req, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle = nullptr) {
            assert_msg(!(lineId != -1 && bcc->isValid(lineId)) || tcc->numSharers(lineId) > 0, "%s: MESIDirectoryHubCC keeps a line with no sharer", name.c_str());
            uint64_t respCycle = startCycle;

            uint32_t fwdId = -1u;
            bool needsParentAccess = false;  // whether a directory GETS/GETX hit cannot use forwarding and requires an access to parent.

            if (req.type == GETS && lineId != -1 && bcc->isValid(lineId)) {
                // GETS hit on the directory.
                needsParentAccess = true;
                if (ChildrenForwarding) {
                    assert(!tcc->isSharer(lineId, req.childId));
                    // When there is an exclusive sharer, the basic protocol sends INVX to represent FWD.
                    // Otherwise we find one sharer to forward.
                    if (!tcc->hasExclusiveSharer(lineId)) {
                        fwdId = findForwarder(lineId, req.childId);
                    }
                    profGETSFwd.inc();
                    needsParentAccess = false;  // forward instead of parent access
                } else {
                    profGETSRly.inc();
                }
            } else if (req.type == GETX && lineId != -1 && bcc->isExclusive(lineId)) {
                // GETX hit on the directory.
                needsParentAccess = true;
                if (!tcc->isSharer(lineId, req.childId)) {
                    // When the requesting child is not a sharer.
                    if (ChildrenForwarding) {
                        // The basic protocol always sends INVs to other children, one of which represents FWD.
                        assert(tcc->numSharers(lineId) > 0);  // at least one INV will be sent (as FWD).
                        profGETXFwdIM.inc();
                        needsParentAccess = false;  // forward instead of parent access
                    } else {
                        profGETXRlyIM.inc();
                    }
                } else {
                    // When the requesting child is a non-exclusive sharer.
                    if (ChildrenForwarding) {
                        // No data need to be forwarded, and the basic protocol sends necessary INVs.
                        assert(!tcc->hasExclusiveSharer(lineId));
                        profGETXFwdSM.inc();
                        needsParentAccess = false;  // forward instead of parent access
                    } else {
                        profGETXRlySM.inc();
                    }
                }
            } else if (req.type == PUTX) {
                // PUTX can only be sent from the last (exclusive) child, will result in an eviction from this level to parents.
                assert(lineId != -1 && tcc->numSharers(lineId) == 1 && tcc->isSharer(lineId, req.childId));
            }
            // Misses are served by the basic protocol.
            // PUTS has no data.

            if (needsParentAccess) {
                // A GETS or GETX hit on the directory.
                // Forwarding cannot be used. Send a no-op relayed access to parent.
                assert(fwdId == -1u);

                auto lineInfo = lineInfoMap.at(req.lineAddr);
                uint32_t parentId = lineInfo.parentId;
                MESIState* state = lineInfo.state;  // use the true state of this level to detect race.
                assert(state);
                assert(bottomLock);
                MESIState curState = *state;
                MemReq rlyReq = {req.lineAddr, req.type, selfId, state, respCycle, bottomLock, curState, req.srcId, req.flags};

                uint32_t nextLevelLat = parents[parentId]->access(rlyReq) - respCycle;
                addLineInfo(rlyReq, parentId);
                uint32_t netLat = parentRTTs[parentId];
                profGETRlyNextLevelLat.inc(nextLevelLat);
                profGETRlyNetLat.inc(netLat);
                respCycle += nextLevelLat + netLat;
            }

            respCycle = MESICC::processAccess(req, lineId, respCycle, getDoneCycle);

            // Forward.
            if (fwdId != -1u) {
                assert(req.type == GETS);  // for now only GETS needs explicit FWD.
                // FWD is only valid for S state. After access we now have both forwardee and forwarder in S state.
                assert(tcc->numSharers(lineId) > 1);
                bool writeback = false;
                InvReq invReq = {req.lineAddr, FWD, &writeback, respCycle, req.srcId};
                respCycle = children[fwdId]->invalidate(invReq);
                assert(!writeback);
                // FWD does not downgrade or invalidate lines in other caches, see MESITopCC::processInval().
                assert(tcc->isSharer(lineId, fwdId));
            }

            // When no children hold the line any more, also evict it from the directory.
            if (lineId != -1 && tcc->numSharers(lineId) == 0) {
                // Check single-record invariant.
                // This can only happen after a PUT, which ends in this level and creates no event thus far.
                assert(req.type == PUTS || req.type == PUTX);
                auto evRec = zinfo->eventRecorders[req.srcId];
                assert(!evRec || !evRec->hasRecord());
                // The following eviction, though, may create an event.
                processEviction(req, req.lineAddr, lineId, respCycle);
                if (unlikely(evRec && evRec->hasRecord())) {
                    // Mark eviction off the critical path.
                    // Note that this writeback happens in CC::processAccess(), so Cache::access() will not mark it.
                    auto wbRec = evRec->popRecord();
                    wbRec.endEvent = nullptr;
                    evRec->pushRecord(wbRec);
                }
            }

            return respCycle;
        }

    protected:
        /* Find a child that can forward the given line to the receiver. */
        uint32_t findForwarder(uint32_t lineId, uint32_t recvId) {
            // Find the closest sharer.
            uint32_t fwdId = -1u;
            auto numChildren = children.size();
            uint32_t right = recvId + 1;
            int32_t left = recvId - 1;  // must be signed.
            while (right < numChildren || left >= 0) {
                if (right < numChildren && tcc->isSharer(lineId, right)) { fwdId = right; break; }
                if (left >= 0 && tcc->isSharer(lineId, left)) { fwdId = left; break; }
                left--;
                right++;
            }
            assert(fwdId != -1u && tcc->isSharer(lineId, fwdId));

            return fwdId;
        }
};

#endif  // CC_EXTS_H_

