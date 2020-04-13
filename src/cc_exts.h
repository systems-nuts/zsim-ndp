#ifndef CC_EXTS_H_
#define CC_EXTS_H_

#include "coherence_ctrls.h"
#include "bithacks.h"
#include "event_recorder.h"
#include "g_std/g_string.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"
#include "locks.h"
#include "memory_hierarchy.h"
#include "network.h"
#include "zsim.h"

/**
 * MESI CC that bypasses, i.e., does not keep, specific lines.
 *
 * For a line that meets the bypass rule, the CC will evict it from the cache as long as all children evict it.
 */
class MESIBypassCC : public MESICC {
    public:
        /* Bypass rules. */

        class BypassRule : public GlobAlloc {
        public:
            virtual bool bypass(const MemReq& req) = 0;
            virtual void initStats(AggregateStat* parentStat) {}
        };

        class NoneBypassRule : public BypassRule {
        public:
            bool bypass(const MemReq&) override { return false; }
        };

        class AllBypassRule : public BypassRule {
        public:
            bool bypass(const MemReq&) override { return true; }
        };

        class BasePartialBypassRule : public BypassRule {
        protected:
            Counter profBypEvicts;
        public:
            void initStats(AggregateStat* parentStat) override {
                profBypEvicts.init("bypassEvicts", "Bypass-introduced evictions");
                parentStat->append(&profBypEvicts);
            }
        };

        class AddressRangeBypassRule : public BasePartialBypassRule {
        protected:
            g_vector<std::pair<Address, Address>> vLineAddrRanges;

        public:
            AddressRangeBypassRule(const g_vector<std::pair<Address, Address>> addrRanges) {
                for (const auto& r : addrRanges) vLineAddrRanges.push_back({r.first << lineBits, r.second << lineBits});
            }

            bool bypass(const MemReq& req) override {
                Address vLineAddr = req.lineAddr ^ procMask;
                for (const auto& r : vLineAddrRanges) {
                    if (r.first <= vLineAddr && vLineAddr < r.second) {
                        profBypEvicts.inc();
                        return true;
                    }
                }
                return false;
            }
        };

    protected:
        BypassRule* bypass;

    public:
        MESIBypassCC(uint32_t _numLines, BypassRule* _bypass, bool _nonInclusiveHack, g_string& _name)
            : MESICC(_numLines, _nonInclusiveHack, _name), bypass(_bypass) {}

        void initStats(AggregateStat* cacheStat) {
            MESICC::initStats(cacheStat);
            bypass->initStats(cacheStat);
        }

        uint64_t processAccess(const MemReq& req, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle = nullptr) {
            uint64_t respCycle = startCycle;
            respCycle = MESICC::processAccess(req, lineId, respCycle, getDoneCycle);

            if (lineId != -1 && tcc->numSharers(lineId) == 0) {
                // Evict bypassing lines when no children hold them any more.
                if (bypass->bypass(req)) {
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
            }

            return respCycle;
        }
};


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
        // Filters repeated access from children, e.g., a directory CC without forwarding.
        const bool filterAcc;
        // Filters no-op invalidations to non-existing lines from parents, e.g., a broadcast CC.
        const bool filterInv;

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
        Counter profGETSRep, profGETXRep;
        Counter profINVNop, profINVXNop;
        Counter profGETRlyNextLevelLat, profGETRlyNetLat;

        PAD();
        lock_t nopStatsLock;  // used for invalidate filtering.
        PAD();

    public:
        MESIDirectoryHubCC(uint32_t _numLines, bool _nonInclusiveHack, bool _filterAcc, bool _filterInv, g_string& _name)
            : MESICC(_numLines, _nonInclusiveHack, _name), filterAcc(_filterAcc), filterInv(_filterInv), selfId(-1u), bottomLock(nullptr)
        {
            futex_init(&nopStatsLock);
        }

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
            profGETSRep.init("repGETS", "Repeated GETS fetches");
            profGETXRep.init("repGETX", "Repeated GETX fetches");
            profINVNop.init("nopINV", "Invalidate non-ops (from upper level)");
            profINVXNop.init("nopINVX", "Downgrade non-ops (from upper level)");
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

            if (filterAcc) {
                cacheStat->append(&profGETSRep);
                cacheStat->append(&profGETXRep);
            }
            if (filterInv) {
                cacheStat->append(&profINVNop);
                cacheStat->append(&profINVXNop);
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
            bool skip = false;  // whether we should skip the access to the current level if it is a repeated access.

            if (req.type == GETS && lineId != -1 && bcc->isValid(lineId)) {
                // GETS hit on the directory.
                needsParentAccess = true;
                if (tcc->isSharer(lineId, req.childId)) {
                    // The child requesting GETS is already a sharer.
                    assert_msg(filterAcc, "%s: encounter a repeated GETS access; did you forget to enable filterAcc?", name.c_str());
                    skip = true;
                    needsParentAccess = false;  // also skip parent access
                    // If there are other sharers, use forwarding. Otherwise leave for access to parent.
                    if (ChildrenForwarding && tcc->numSharers(lineId) > 1) {
                        fwdId = findForwarder(lineId, req.childId);
                        profGETSFwd.inc();
                    } else {
                        profGETSRep.inc();
                    }
                } else if (ChildrenForwarding) {
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
                if (tcc->hasExclusiveSharer(lineId) && tcc->isSharer(lineId, req.childId)) {
                    // The child requesting GETX is already the exclusive sharer.
                    assert_msg(filterAcc, "%s: encounter a repeated GETX access; did you forget to enable filterAcc?", name.c_str());
                    skip = true;
                    needsParentAccess = false;  // also skip parent access
                    // No other sharers, no forwarding.
                    // Here we do not know what the child state should be, i.e., not distinguish I->M and S->M.
                    profGETXRep.inc();
                } else if (!tcc->isSharer(lineId, req.childId)) {
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

                /* NOTE(gaomy):
                 *
                 * Here a race may happen because we issue an access to parent and will release our bottom lock shortly
                 * before the access. An intermediate invalidate may come in and cause the line states in this level as
                 * well as any child levels to be invalid.
                 *
                 * In such case, we make sure that the relayed access could become an actual access to the parent if
                 * needed, to restore the state in this level. This includes:
                 * - use the true state in the CC of this level in the relayed access to the parent.
                 * - treat filterAcc as a sanity check rather than a condition, so an access can adaptively switch
                 *   between a relayed one or an actual one.
                 * - check the state after access, to make sure it is restored to a valid state.
                 *
                 * Also, then the access to this level (after the access to parent) should not be skipped, and would
                 * handle the states of child levels.
                 */

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

                // Make sure the state of this level is restored (may not to the same original state, but a valid hit).
                assert((req.type == GETS && *state != I) || (req.type == GETX && (*state == M || *state == E)));

                // Do not skip the access to this level.
                assert(!skip);
            }

            if (!skip) respCycle = MESICC::processAccess(req, lineId, respCycle, getDoneCycle);

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

        bool startInv(const InvReq& req) {
            // Filter invalidates.
            if (req.writeback == nullptr /* FIXME: use a new InvType. */) {
                assert_msg(filterInv, "%s: encounter a broadcast invalidates; did you forget to enable filterInv?", name.c_str());
                // No-op invalidation.
                futex_lock(&nopStatsLock);
                if (req.type == INV) {
                    profINVNop.inc();
                } else if (req.type == INVX) {
                    profINVXNop.inc();
                } else {
                    assert(req.type == FWD);
                    // FWD replaces INVX when already non-exclusive. See MESIBroadcastCC::broadcastNopInv().
                    profINVXNop.inc();
                }
                futex_unlock(&nopStatsLock);
                return true;
            }

            bool skipInv = MESICC::startInv(req);

            if (!skipInv && req.type == INV) assert(removeLineInfo(req.lineAddr) == 1);

            return skipInv;
        }

        bool startAccess(MemReq& req) {
            if (filterAcc) {
                // These checks should always match when MESIBottomCC::processAccess() does NOT issue parent accesses.
                if ((req.type == GETS && req.initialState != I) ||
                        (req.type == GETX && (req.initialState == M || req.initialState == E))) {
                    // This is a repeated access expected to be filtered, which looks invalid to the race check logic. We need to fool it.
                    MESIState dummyState = I;
                    MemReq dummyReq = req;
                    dummyReq.state = &dummyState;
                    dummyReq.initialState = dummyState;
                    // We still have the child lock, state should not change.
                    assert(req.initialState == *req.state);
                    assert(MESICC::startAccess(dummyReq) == false);
                    // Now we have properly locked.
                    assert(dummyReq.type == req.type);
                    return false;
                }
            }
            return MESICC::startAccess(req);
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


/**
 * MESI CC that does not use directory to track sharers.
 *
 * For access, it forwards to parents; for invalidation, it broadcasts to all children.
 *
 * Require parents to filter repeated accesses, and children to filter no-op invalidations.
 */
class MESIBroadcastHubCC : public MESIDirectoryHubCC<false> {
    public:
        typedef MESIDirectoryHubCC<false> BaseCC;

    protected:
        const uint32_t banksPerChild;

    public:
        MESIBroadcastHubCC(uint32_t _numLines, uint32_t _banksPerChild, bool _nonInclusiveHack, bool _filterAcc, bool _filterInv, g_string& _name)
            : BaseCC(_numLines, _nonInclusiveHack, _filterAcc, _filterInv, _name), banksPerChild(_banksPerChild) {}

        uint64_t processAccess(const MemReq& req, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle = nullptr) {
            // Check whether the requesting child has sufficient permission or we need to broadcast.
            InvType broadcastType = FWD;  // FWD is a placeholder for no broadcast
            if (req.type == GETS && !tcc->isSharer(lineId, req.childId)) {
                // Broadcast unless the requesting child is already a sharer.
                broadcastType = INVX;
            } else if (req.type == GETX && !(tcc->hasExclusiveSharer(lineId) && tcc->isSharer(lineId, req.childId))) {
                // Broadcast unless the requesting child is already the exclusive sharer.
                broadcastType = INV;
            }
            // No broadcast for PUTs.

            // Use basic protocol for parent access and sharer children invalidation.
            uint64_t _getDoneCycle = startCycle;
            uint64_t respCycle = BaseCC::processAccess(req, lineId, startCycle, &_getDoneCycle);
            if (getDoneCycle) *getDoneCycle = _getDoneCycle;

            // Additional no-op invalidation broadcast to non-sharer children.
            uint64_t nopInvDoneCycle = _getDoneCycle;
            if (broadcastType != FWD) {
                nopInvDoneCycle = broadcastNopInv(req.lineAddr, broadcastType, req.childId, lineId, _getDoneCycle, req.srcId);
            }

            respCycle = MAX(respCycle, nopInvDoneCycle);
            return respCycle;
        }

        uint64_t processInv(const InvReq& req, int32_t lineId, uint64_t startCycle) {
            // Use basic protocol for sharer children invalidation.
            uint64_t respCycle = BaseCC::processInv(req, lineId, startCycle);

            // Additional no-op invalidation broadcast to non-sharer children.
            uint64_t nopInvDoneCycle = startCycle;
            if (req.type != FWD) {
                // FWD does not propagate up.
                nopInvDoneCycle = broadcastNopInv(req.lineAddr, req.type, -1, lineId, startCycle, req.srcId);
            }

            respCycle = MAX(respCycle, nopInvDoneCycle);
            return respCycle;
        }

    protected:
        uint64_t broadcastNopInv(Address lineAddr, InvType type, uint32_t reqChildId, int32_t lineId, uint64_t startCycle, uint32_t srcId) {
            assert(type == INV || type == INVX);
            uint32_t respCycle = startCycle;

            // Go through children and send no-op invalidations.
            // Only one invalidation needs to be sent to each multi-bank child, i.e., to the same offset in each child
            // if assuming all children use the same address mapping. In any case, this is a no-op and will not cause
            // any correctness issue.
            uint32_t offset = reqChildId % banksPerChild;
            for (uint32_t c0 = 0; c0 < children.size(); c0 += banksPerChild) {
                uint32_t c = c0 + offset;
                // Skip the requesting child.
                if (c == reqChildId) continue;
                // Skip sharers, which are handled by the basic protocol.
                // We need to check all banks within this child.
                uint32_t s = 0;
                for (uint32_t i = 0; i < banksPerChild; i++) if (tcc->isSharer(lineId, c0 + i)) s++;
                if (s) {
                    assert_msg(s == 1, "%s: %u different banks of the same child all have the line; did you specify the correct banksPerChild?", name.c_str(), s);
                    continue;
                }

                // Use invalid writeback pointer, so that a no-op invalidation will not fail silently if not properly filtered.
                InvReq req = {lineAddr, type, nullptr, startCycle, srcId};
                uint64_t cycle = children[c]->invalidate(req);
                respCycle = MAX(respCycle, cycle);
            }

            // The basic protocol will ignore INVX if children are already non-exclusive. See MESITopCC::sendInvalidates().
            // In this case the line is in S states in the children. We use no-op FWD.
            if (type == INVX && !tcc->hasExclusiveSharer(lineId)) {
                for (uint32_t c = 0; c < children.size(); c++) {
                    if (tcc->isSharer(lineId, c)) {
                        InvReq req = {lineAddr, FWD, nullptr, startCycle, srcId};
                        uint64_t cycle = children[c]->invalidate(req);
                        respCycle = MAX(respCycle, cycle);
                    }
                }
            }

            return respCycle;
        }
};

#endif  // CC_EXTS_H_

