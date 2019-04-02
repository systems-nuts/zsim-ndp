#ifndef CC_EXTS_H_
#define CC_EXTS_H_

#include "coherence_ctrls.h"
#include "event_recorder.h"
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "zsim.h"

/**
 * MESI CC for a coherence directory hub with no actual data storage.
 *
 * The CC maintains a directory of sharers of cachelines, and uses this directory to maintain coherence among children.
 * It can be used as a dummy parent without actually introducing another cache level.
 *
 * On data access from a child, if any of the other children have already cached the requested data, the closest child
 * to the requesting child is asked to serve the data by forwarding. Otherwise the next parent level serves the data.
 * No data hit at this level.
 */
class MESIDirectoryHubCC : public MESICC {
    protected:
        // Children.
        g_vector<BaseCache*> children;

        Counter profGETSFwd, profGETXFwdIM, profGETXFwdSM;

    public:
        MESIDirectoryHubCC(uint32_t _numLines, bool _nonInclusiveHack, g_string& _name)
            : MESICC(_numLines, _nonInclusiveHack, _name) {}

        void setChildren(const g_vector<BaseCache*>& _children, Network* network) {
            children.assign(_children.begin(), _children.end());
            MESICC::setChildren(_children, network);
        }

        void initStats(AggregateStat* cacheStat) {
            profGETSFwd.init("fwdGETS", "GETS forwards");
            profGETXFwdIM.init("fwdGETXIM", "GETX I->M forwards");
            profGETXFwdSM.init("fwdGETXSM", "GETX S->M forwards (upgrade forwards)");

            cacheStat->append(&profGETSFwd);
            cacheStat->append(&profGETXFwdIM);
            cacheStat->append(&profGETXFwdSM);

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
        }

        uint64_t processAccess(const MemReq& req, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle = nullptr) {
            assert_msg(!(lineId != -1 && bcc->isValid(lineId)) || tcc->numSharers(lineId) > 0, "%s: MESIDirectoryHubCC keeps a line with no sharer", name.c_str());
            uint64_t respCycle = startCycle;

            uint32_t fwdId = -1u;

            if (req.type == GETS && lineId != -1 && bcc->isValid(lineId)) {
                // GETS hit on the directory.
                assert(!tcc->isSharer(lineId, req.childId));
                // When there is an exclusive sharer, the basic protocol sends INVX to represent FWD.
                // Otherwise we find one sharer to forward.
                if (!tcc->hasExclusiveSharer(lineId)) {
                    fwdId = findForwarder(lineId, req.childId);
                }
                profGETSFwd.inc();
            } else if (req.type == GETX && lineId != -1 && bcc->isExclusive(lineId)) {
                // GETX hit on the directory.
                if (!tcc->isSharer(lineId, req.childId)) {
                    // When the requesting child is not a sharer.
                    // The basic protocol always sends INVs to other children, one of which represents FWD.
                    assert(tcc->numSharers(lineId) > 0);  // at least one INV will be sent (as FWD).
                    profGETXFwdIM.inc();
                } else {
                    // When the requesting child is a non-exclusive sharer.
                    // No data need to be forwarded, and the basic protocol sends necessary INVs.
                    assert(!tcc->hasExclusiveSharer(lineId));
                    profGETXFwdSM.inc();
                }
            }
            // Misses are served by the basic protocol.
            // PUTS and PUTX need no forwarding.

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

