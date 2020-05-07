#ifndef MEM_INTERCONNECT_EVENT_RECORDER_H_
#define MEM_INTERCONNECT_EVENT_RECORDER_H_

#include "bithacks.h"
#include "event_recorder.h"
#include "g_std/g_vector.h"
#include "intrusive_list.h"
#include "mem_router.h"
#include "slab_alloc.h"
#include "timing_event.h"

/**
 * A recorder for invalidate events generated in MemInterconnect.
 *
 * NOTE(gaomy):
 *
 * zsim currently assumes that invalidate does not create any event. So we sometimes have to pop the events and not
 * leave them in the event recorder, e.g., when an eviction causes some invalidates, and then needs to do a PUT access.
 * In that sense, the timing event information for invalidates will become incomplete.
 *
 * Because invalidates are always associated with an access, we can stash the interconnect travel events generated in
 * invalidate(), and merge with the access events when returning to access(). This recorder is used for such stashing
 * purpose.
 *
 * Invalidates may be nested, i.e., a child who gets invalidated by its parent may further invalidate its own children.
 * All invalidates sent out from the same parent level are in parallel, but must all finish before the invalidate to the
 * parent finishes.
 *
 * In addition, we optimistically (i.e., may underestimate latencies) make invalidate travel events be in parallel with
 * access events. This should be OK because (1) for eviction-introduced invalidates, the invalidates and the writebacks
 * can be treated as off the critical path; (2) for access-introduced invalidates, the invalidates are used to recall
 * only permissions (unless dirty).
 *
 * However, when merging with the associated access, we do require the invalidate events finish before the access
 * returns, since the access possibly needs to wait for the dirty lines that the invalidates write back.
 */
class MemInterconnectEventRecorder : public GlobAlloc {
    private:
        struct RoutingEntry : InListNode<RoutingEntry> {
            MemRouter* router;
            uint8_t portId;
            uint8_t procDelay;
            uint16_t outDelay;
            uint16_t preDelay;
            RoutingEntry(MemRouter* _router, uint32_t _portId, uint32_t _procDelay, uint32_t _outDelay, uint32_t _preDelay) {
                assert(_portId < (1u << 8));
                assert(_procDelay < (1u << 8));
                assert(_outDelay < (1u << 16));
                assert(_preDelay < (1u << 16));
                router = _router;
                portId = static_cast<uint8_t>(_portId);
                procDelay = static_cast<uint8_t>(_procDelay);
                outDelay = static_cast<uint16_t>(_outDelay);
                preDelay = static_cast<uint16_t>(_preDelay);
            }
            void* operator new (size_t sz, slab::SlabAlloc& a) { return a.alloc(sz); }
            void operator delete(void*, size_t) { panic("RoutingEntry::delete should never be called"); }
            void operator delete (void* p, slab::SlabAlloc& a) { panic("RoutingEntry::delete PLACEMENT delete called"); }
        };  // Size matters as we may generate A LOT OF events in invalidates.
        slab::SlabAlloc reAlloc;  // use slab allocator for routing entries instead of g_vector, to avoid slow allocation.

        /**
         * An aggregated event for all routing events during the interconnect travel along one trip.
         */
        class MemInterconnectEvent : public TimingEvent {
        private:
            const uint64_t id;
            uint64_t minDoneCycle;
            InList<RoutingEntry> entries;

        public:
            MemInterconnectEvent(uint64_t _id, uint64_t startCycle, int32_t domain = -1)
                : TimingEvent(0, 0, domain), id(_id), minDoneCycle(startCycle), entries()
            {
                setMinStartCycle(startCycle);
            }

            // Bound phase.

            void addHop(RoutingEntry* e, uint64_t doneCycle) {
                assert(doneCycle >= minDoneCycle);
                minDoneCycle = doneCycle;
                entries.push_back(e);
            }

            uint64_t getDoneCycle() { return minDoneCycle; }

            // Weave phase.

            void simulate(uint64_t startCycle) {
                auto e = entries.front();
                entries.pop_front();  // does not invalidate the popped entry
                bool lastHop = entries.empty();
                uint64_t doneCycle = startCycle;
                if (e) {
                    doneCycle += e->preDelay;
                    doneCycle = e->router->simulate(e->portId, e->procDelay, e->outDelay, lastHop, doneCycle);
                    // info("[MemInterconnectEvent %lu] Hop at %s port %u, starts at %lu, finishes at %lu", id, e->router->getName(), e->portId, startCycle, doneCycle);
                    slab::freeElem(e, sizeof(RoutingEntry));
                }
                if (lastHop) {  // all done
                    assert(doneCycle >= minDoneCycle);
                    done(doneCycle);
                } else {  // more hops to do
                    requeue(doneCycle);
                }
            }
        };
        MemInterconnectEvent* event;
        uint64_t eventId;

        EventRecorder* evRec;
        g_vector<TimingRecord> invStashRecs;

        /**
         * A stack to record nested accesses/invalidates.
         *
         * When traveling along the request path of an access or invalidate, an entry with the interconnect event of the
         * request travel is pushed onto the stack, in order to prepare for any nested accesses or invalidates.
         *
         * During the nested accesses and invalidates, dependent events are linked to the parent event on the stack.
         *
         * When traveling along the response path, the dependencies are respected and the entry is popped. The
         * interconnect event of the response travel is linked after these dependencies.
         */
        struct StackEntry {
            TimingRecord tr;
            TimingEvent* doneEv;
            uint64_t doneCycle;

            StackEntry() : doneEv(nullptr), doneCycle(0) { tr.clear(); }

            // Entry for access.
            StackEntry(TimingEvent* ev, uint64_t cycle, Address lineAddr, AccessType type) : StackEntry() {
                tr.addr = lineAddr; tr.type = type;
                tr.startEvent = tr.endEvent = ev;
                tr.reqCycle = tr.respCycle = cycle;
                assert(isAccess());
            }

            // Entry for invalidate.
            StackEntry(TimingEvent* ev, uint64_t cycle) : StackEntry() {
                tr.endEvent = ev;
                tr.reqCycle = tr.respCycle = cycle;
                assert(!isAccess());
            }

            inline bool isAccess() const { return tr.isValid(); }

            // Add a dependency.
            void addDep(EventRecorder* evRec, TimingEvent* ev, uint64_t respCycle) {
                if (!doneEv) {
                    // Min start cycle of doneEv is doneCycle; will be set later.
                    doneEv = new (evRec) DelayEvent(0);
                }

                // Add dependency to the done event.
                assert(ev);
                ev->addChild(doneEv, evRec);  // directly link without delay.
                doneCycle = MAX(doneCycle, respCycle);
            }

            // Merge all dependencies and return the min start cycle for future events.
            uint64_t mergeDeps(EventRecorder* evRec) {
                if (doneEv) {
                    // Link with the previous end event.
                    if (tr.endEvent) tr.endEvent->addChild(doneEv, evRec);  // directly link without delay.
                    doneCycle = MAX(doneCycle, tr.respCycle);

                    // Mark the done event as the new end event.
                    doneEv->setMinStartCycle(doneCycle);
                    tr.endEvent = doneEv;
                    tr.respCycle = doneCycle;
                }
                return tr.respCycle;
            }
        };
        g_vector<StackEntry> stack;
        uint32_t sp;  // point to the empty slot above the stack top

        const uint32_t domain;

    private:
        // Link two events \c prev and \c next, and properly handle the delay in between. Return the end event of the chain.
        // The event \c next could be null, in which case the chain will extend to the returned end event at the cycle of nextReqCycle.
        TimingEvent* linkEvents(TimingEvent* prev, TimingEvent* next, uint64_t prevRespCycle, uint64_t nextReqCycle) {
            assert(nextReqCycle >= prevRespCycle);
            TimingEvent* end = prev;
            if (nextReqCycle > prevRespCycle) {
                auto dEv = new (evRec) DelayEvent(nextReqCycle - prevRespCycle);
                dEv->setMinStartCycle(prevRespCycle);
                prev->addChild(dEv, evRec);
                end = dEv;
            }
            if (next) {
                end->addChild(next, evRec);
                end = next;
            }
            return end;
        }

        // Event management.

        inline void startMemInterconnectEvent(uint64_t cycle) {
            assert(event == nullptr);
            event = new (evRec) MemInterconnectEvent(eventId++, cycle, domain);
        }

        inline void endMemInterconnectEvent() {
            event = nullptr;
        }

        // Stack management.

        inline StackEntry& stackTop() {
            assert(sp > 0);
            return stack[sp - 1];
        }

        inline void stackPop() {
            assert(sp > 0);
            sp--;
        }

        template<typename... Args>
        inline void stackPush(Args... args) {
            if (unlikely(sp == stack.size())) {
                stack.resize(2 * stack.size());
            }
            stack[sp] = StackEntry(args...);
            sp++;
        }

    public:
        MemInterconnectEventRecorder(EventRecorder* _evRec, uint32_t _domain)
            : reAlloc(), event(nullptr), eventId(0), evRec(_evRec), invStashRecs(),
              stack(16), sp(0), domain(_domain) {}

        void addHop(MemRouter* router, uint32_t portId, uint32_t procDelay, uint32_t outDelay, uint64_t startCycle, uint64_t doneCycle) {
            auto e = new (reAlloc) RoutingEntry(router, portId, procDelay, outDelay, startCycle - event->getDoneCycle());
            event->addHop(e, doneCycle);
        }

        // Before staring access request travel.
        template<bool isAcc = true>
        void startRequest(uint64_t cycle, Address lineAddr, AccessType type) {
            static_assert(isAcc, "Called wrong method for accesses/invalidates");
            if (!evRec) return;

            // Create an interconnect event for the request travel.
            startMemInterconnectEvent(cycle);

            // The dependency with respect to any parent access that is currently on the stack will be handled when
            // starting the response travel of the parent access, i.e., the events generated during this child access
            // will be put into the event recorder and treated as the events generated in a nested access.

            // Push a new entry for access to stack.
            stackPush(event, cycle, lineAddr, type);
        }

        // Before starting invalidate request travel.
        template<bool isAcc = false>
        void startRequest(uint64_t cycle) {
            static_assert(!isAcc, "Called wrong method for accesses/invalidates");
            if (!evRec) return;

            // Pop the previous access record.
            // We do not link events with the previous access record, since in the case of multiple parallel invalidates, the
            // previous access record may be from another invalidate and does not have dependency with this one.
            TimingRecord rec;
            rec.clear();
            if (evRec->hasRecord()) {
                rec = evRec->popRecord();
            }
            invStashRecs.push_back(rec);

            // Create an interconnect event for the request travel.
            startMemInterconnectEvent(cycle);

            // The current stack top entry is for the parent access/invalidate that generates this invalidate.
            // Link the start of this invalidate request travel to the end of the request travel of the parent, which is
            // the current end event of the parent entry.
            const auto& parentRec = stackTop().tr;
            linkEvents(parentRec.endEvent, event, parentRec.respCycle, cycle);

            // Push a new entry for invalidate to stack.
            stackPush(event, cycle);
        }

        // After finishing access/invalidate request travel.
        template<bool isAcc>
        void endRequest(uint64_t cycle) {
            if (!evRec) return;

            // Record the end of the request travel.
            auto& rec = stackTop().tr;
            assert(rec.endEvent == event);
            rec.endEvent = linkEvents(event, nullptr, event->getDoneCycle(), cycle);
            rec.respCycle = cycle;

            endMemInterconnectEvent();
        }

        // Before starting access/invalidate response travel.
        // Merge dependencies from nested accesses/invalidates. Return updated response start cycle.
        template<bool isAcc>
        uint64_t startResponse(uint64_t cycle) {
            if (!evRec) return cycle;

            // The current stack top entry is up to the end of the request travel associated with this response.
            auto& e = stackTop();

            // Link with any event generated during nested access/invalidate.
            if (evRec->hasRecord()) {
                auto rec = evRec->popRecord();
                // Happen after the end of the request travel.
                // Link after the current end event of the current stack top entry.
                linkEvents(e.tr.endEvent, rec.startEvent, e.tr.respCycle, rec.reqCycle);
                if (rec.endEvent) {
                    e.tr.endEvent = rec.endEvent;
                    e.tr.respCycle = rec.respCycle;
                }  // otherwise the nested access/invalidate is off critical path.
            }

            // Merge all dependencies before the start of the response travel.
            cycle = MAX(cycle, e.mergeDeps(evRec));

            // Create an interconnect event for the response travel.
            startMemInterconnectEvent(cycle);

            // Link with the previous end event.
            e.tr.endEvent = linkEvents(e.tr.endEvent, event, e.tr.respCycle, cycle);
            e.tr.respCycle = cycle;

            return cycle;
        }

        // After finishing access/invalidate response travel.
        // Make an access record for access, or mark as a dependency for invalidate.
        template<bool isAcc>
        void endResponse(uint64_t cycle) {
            if (!evRec) return;

            // Pop the current stack top entry.
            StackEntry e = stackTop();  // copy
            assert_msg(e.isAccess() == isAcc, "MemInterconnectEventRecorder: unmatched push/pop from access/invalidate");
            stackPop();

            // Record the end of the request travel.
            assert(e.tr.endEvent == event);
            e.tr.endEvent = linkEvents(event, nullptr, event->getDoneCycle(), cycle);
            e.tr.respCycle = cycle;

            if (isAcc) {
                // For access, make a record and put into event recorder.
                evRec->pushRecord(e.tr);
            } else {
                // For invalidates, mark events as one dependency of the parent record.
                stackTop().addDep(evRec, e.tr.endEvent, e.tr.respCycle);
            }

            endMemInterconnectEvent();

            if (!isAcc) {
                // Push back the previous access record if any.
                TimingRecord rec = invStashRecs.back();
                if (rec.isValid()) {
                    evRec->pushRecord(rec);
                }
                invStashRecs.pop_back();
            }
        }

        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

#endif  // MEM_INTERCONNECT_EVENT_RECORDER_H_
