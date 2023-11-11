#include "mem_channel.h"
#include "contention_sim.h"
#include "timing_event.h"
#include "zsim.h"

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

// Weave phase event for one memory access request.
class MemChannelAccEvent : public TimingEvent {
    private:
        MemChannel* mem;
        Address addr;
        uint32_t data_size;
        bool write;

    public:
        MemChannelAccEvent(MemChannel* _mem, bool _isWrite, Address _addr, uint32_t _data_size, 
                int32_t domain, uint32_t preDelay, uint32_t postDelay)
            : TimingEvent(preDelay, postDelay, domain), mem(_mem), addr(_addr), 
              data_size(_data_size), write(_isWrite) {}

        Address getAddr() const { return addr; }
        bool isWrite() const { return write; }
        uint32_t get_data_size() const { return data_size; }

        void simulate(uint64_t startCycle) {
            mem->acceptAccEvent(this, startCycle);
        }
};

// Weave phase event for memory system tick.
class MemChannelTickEvent : public TimingEvent, public GlobAlloc {
    private:
        MemChannel* const mem;
        enum State { IDLE, QUEUED, RUNNING, ANNULLED } state;

    public:
        MemChannelTickEvent(MemChannel* _mem, int32_t domain)
            : TimingEvent(0, 0, domain), mem(_mem)
        {
            setMinStartCycle(0);
            setRunning();
            state = IDLE;
            hold();
        }

        void parentDone(uint64_t startCycle) {
            panic("This is queued directly");
        }

        void simulate(uint64_t startCycle) {
            assert(state == QUEUED || state == ANNULLED);
            if (state == QUEUED) {
                state = RUNNING;
                uint64_t nextCycle = mem->tick(startCycle);
                if (nextCycle) {
                    assert(nextCycle >= startCycle);
                    requeue(nextCycle);
                    state = QUEUED;
                    return;
                }
            }
            // Recycle this event.
            state = IDLE;
            hold();
            mem->recycleTickEvent(this);
        }

        void enqueue(uint64_t cycle) {
            assert(state == IDLE);
            state = QUEUED;
            // Enqueue happens in weave phase
            requeue(cycle);
        }

        void annul() {
            assert(state == QUEUED);
            state = ANNULLED;
        }

        // Use glob mem
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

// Weave phase event for memory system periodical process.
class MemChannelPeriodicalEvent : public TimingEvent, public GlobAlloc {
    private:
        MemChannel* const mem;
        const uint32_t index;
        const uint64_t interval;  // in sys cycles.

    public:
        MemChannelPeriodicalEvent(MemChannel* _mem, uint32_t _index, uint64_t _interval, int32_t domain)
            : TimingEvent(0, 0, domain), mem(_mem), index(_index), interval(_interval)
        {
            setMinStartCycle(0);
            if (interval != -1uL) {
                queue(interval);
                DEBUG("Periodical event created, interval is %lu", interval);
            } else {
                DEBUG("Periodical event created but will be ignored");
            }
        }

        void parentDone(uint64_t startCycle) {
            panic("This is queued directly");
        }

        void simulate(uint64_t startCycle) {
            mem->periodicalTick(startCycle, index);
            requeue(startCycle + interval);
        }

        // Use glob mem
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

MemChannel::MemChannel(MemChannelBackend* _be, const uint32_t _sysFreqMHz, const uint32_t _controllerSysDelay,
        bool _waitForWriteAck,
        const uint32_t _domain, const g_string& _name)
    : name(_name), domain(_domain), be(_be), controllerSysDelay(_controllerSysDelay), sysFreqKHz(_sysFreqMHz * 1000),
      waitForWriteAck(_waitForWriteAck), tickCycle(-1uL), tickEvent(nullptr), freeTickEvent(nullptr)
{
    memFreqKHz = be->getMemFreqKHz();
    // minRdDelay = memToSysCycle(be->getMinLatency(false));
    // minWrDelay = memToSysCycle(be->getMinLatency(true));
    // Memory ctrl delay is pre the contention.
    // preRdDelay = preWrDelay = controllerSysDelay;
    // postRdDelay = minRdDelay - preRdDelay;
    // postWrDelay = minWrDelay - preWrDelay;

    // Allocate periodical event from global space. Will be automatically reclaimed.
    for (uint32_t index = 0; index < be->getPeriodicalEventCount(); index++) {
        uint64_t memInterval = be->getPeriodicalInterval(index);
        new MemChannelPeriodicalEvent(this, index, matchingSysCycle(memInterval), domain);
    }
}

void MemChannel::initStats(AggregateStat* parentStat) {
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Memory channel stats");

    profReads.init("rd", "Read requests");
    memStats->append(&profReads);
    profWrites.init("wr", "Write requests");
    memStats->append(&profWrites);
    profTotalRdLat.init("rdlat", "Total latency experienced by read requests");
    memStats->append(&profTotalRdLat);
    profTotalWrLat.init("wrlat", "Total latency experienced by write requests");
    memStats->append(&profTotalWrLat);
    rdLatencyHist.init("rdmlh", "Latency histogram for read requests", NUMBINS);
    memStats->append(&rdLatencyHist);
    wrLatencyHist.init("wrmlh", "Latency histogram for write requests", NUMBINS);
    memStats->append(&wrLatencyHist);

    // Backend stats.
    be->initStats(memStats);

    parentStat->append(memStats);
}

uint64_t MemChannel::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL) ? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }

    if (req.type == PUTS) {
        return req.cycle; // zero latency
    } else {
        bool isWrite = (req.type == PUTX);
        uint64_t respCycle = req.cycle + (isWrite ? minWrDelay : minRdDelay);
        if (zinfo->eventRecorders[req.srcId]) {
            MemChannelAccEvent* memEv = new (zinfo->eventRecorders[req.srcId])
                MemChannelAccEvent(this, isWrite, req.lineAddr, 64U, domain,
                        isWrite ? preWrDelay : preRdDelay,
                        isWrite ? postWrDelay : postRdDelay);
            memEv->setMinStartCycle(req.cycle);
            TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
            zinfo->eventRecorders[req.srcId]->pushRecord(tr);
        }
        return respCycle;
    }
}

uint64_t MemChannel::access(MemReq& req, bool isCritical, uint32_t data_size) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL) ? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }
    // assert(data_size == this->burstCount * this->channelWidth)
    assert(data_size >= 8); // at least 8B unit

    if (req.type == PUTS) {
        return req.cycle; // zero latency
    } else {
        bool isWrite = (req.type == PUTX);
        // TBY TODO: memory access size
        uint64_t respCycle = req.cycle + (isWrite? minWrDelay : minRdDelay)/*  + memToSysCycle(data_size - 8) */;
        // uint64_t respCycle = req.cycle + getMinDelay(isWrite, data_size);
        EventRecorder* eventRec = zinfo->eventRecorders[req.srcId];
        if (eventRec) {
            MemChannelAccEvent* memEv = new (eventRec)
            MemChannelAccEvent(this, isWrite, req.lineAddr, data_size, domain,
                    isWrite ? preWrDelay : preRdDelay,
                    isWrite ? postWrDelay : postRdDelay);
            if (eventRec->hasRecord()) {
                TimingRecord tr = eventRec->popRecord();
                memEv->setMinStartCycle(req.cycle);
                assert(req.cycle >= tr.respCycle);
                DelayEvent* dr = new (eventRec) DelayEvent(req.cycle - tr.respCycle);
                dr->setMinStartCycle(tr.respCycle);
                tr.endEvent->addChild(dr, eventRec)->addChild(memEv, eventRec);

                tr.endEvent = memEv;
                tr.respCycle = respCycle;
                eventRec->pushRecord(tr);
            } else {
                memEv->setMinStartCycle(req.cycle);
                TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
                zinfo->eventRecorders[req.srcId]->pushRecord(tr);
            }
        } else {
            panic("no event recorder!");
        }
        return respCycle;
    }
}

void MemChannel::acceptAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle) {
    DEBUG("Accept AccEvent to 0x%lx at sysCycle %lu", ev->getAddr(), sysCycle);

    // Hold the access event for further scheduling/issuing. Release when responding.
    ev->hold();

    // Request queue overflow check.
    if (be->queueOverflow(ev->isWrite())) {
        overflowQueue.push_back(std::make_pair(ev, sysCycle));
        DEBUG("Event added to overflow queue, queue size %lu", overflowQueue.size());
        return;
    }

    // Schedule.
    uint64_t memCycle = sysToMemCycle(sysCycle);
    DEBUG("Schedule new request (0x%lx,%c) at %lu", ev->getAddr(), ev->isWrite()?'w':'r', memCycle);
    uint64_t estTickCycle = schedule(ev, sysCycle, memCycle);

    // Adjust the tick cycle since the new request may be able to issued earlier.
    if (estTickCycle < tickCycle) {
        tickCycle = estTickCycle;

        // Annul the current tick event.
        if (tickEvent) tickEvent->annul();
        // Pick and set the new tick event.
        if (freeTickEvent != nullptr) {
            tickEvent = freeTickEvent;
            freeTickEvent = nullptr;
        } else {
            tickEvent = new MemChannelTickEvent(this, domain);
        }
        uint64_t enqSysCycle = matchingSysCycle(tickCycle);
        assert(enqSysCycle >= sysCycle);
        tickEvent->enqueue(enqSysCycle);
        DEBUG("Tick cycle shifted to %lu by new request", tickCycle);
    }
}

void MemChannel::respondAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle) {
    bool isWrite = ev->isWrite();
    ev->release();
    DEBUG("Respond AccEvent to 0x%lx at sysCycle %lu", ev->getAddr(), sysCycle);
    // Exclude the post delay from the done cycle.
    ev->done(sysCycle - (isWrite ? postWrDelay : postRdDelay));
}

uint64_t MemChannel::tick(uint64_t sysCycle) {
    uint64_t memCycle = sysToMemCycle(sysCycle);
    assert_msg(memCycle == tickCycle, "Tick at wrong time %lu (should be %lu)", memCycle, tickCycle);
    DEBUG("Tick at %lu", memCycle);

    // Try issue and get the next tick cycle.
    uint64_t nextTickCycle = issue(memCycle);
    assert(nextTickCycle > memCycle);

    // Try to schedule an overflow request.
    if (!overflowQueue.empty()) {
        auto& oe = overflowQueue.front();
        MemChannelAccEvent* ev = oe.first;
        uint64_t startCycle = oe.second;
        if (!be->queueOverflow(ev->isWrite())) {
            DEBUG("Schedule overflow request (0x%lu,%c) at %lu", ev->getAddr(), ev->isWrite()?'w':'r', memCycle);
            uint64_t estTickCycle = schedule(ev, startCycle, memCycle);
            overflowQueue.pop_front();
            DEBUG("Event removed from overflow queue, queue size %lu", overflowQueue.size());
            if (estTickCycle < nextTickCycle) {
                // The new scheduled request could make tick cycle earlier.
                nextTickCycle = estTickCycle;
                DEBUG("Tick event shifted to %lu by overflow request", nextTickCycle);
            }
        }
    }

    // Update tick cycle.
    tickCycle = nextTickCycle;
    if (tickCycle == -1uL) {
        tickEvent = nullptr;
        // Check: if there is any AccEvent in the queues, should not have infinite tick cycle!
        assert(be->queueEmpty(true) && be->queueEmpty(false));
        return 0; // 0 means no more event.
    }
    return std::max(sysCycle, matchingSysCycle(tickCycle)); // make sure no go back in time.
}

void MemChannel::recycleTickEvent(MemChannelTickEvent* tev) {
    assert(tev != tickEvent);
    if (freeTickEvent == nullptr) freeTickEvent = tev;
    else delete tev;
}

void MemChannel::periodicalTick(uint64_t sysCycle, uint32_t index) {
    be->periodicalProcess(sysToMemCycle(sysCycle), index);
}

uint64_t MemChannel::schedule(MemChannelAccEvent* ev, uint64_t startCycle, uint64_t memCycle) {
    auto respEv = ev;
    if (ev->isWrite() && !waitForWriteAck) {
        // Respond write request immediately when scheduling.
        respondAccEvent(ev, memToSysCycle(memCycle));
        respEv = nullptr;
    }
    return be->enqueue(ev->getAddr(), ev->isWrite(), startCycle, memCycle, respEv);
}

uint64_t MemChannel::issue(uint64_t memCycle) {
    uint64_t minTickCycle = -1uL;
    MemChannelAccReq* req = be->dequeue(memCycle, &minTickCycle);
    if (!req) {
        // If no request is ready, return minimum tick cycle as the next tick cycle.
        return minTickCycle;
    }
    DEBUG("Issue AccReq to 0x%lx at %lu", req->addr, memCycle);

    // Process the request.
    uint64_t respCycle = be->process(req);
    uint64_t sysRespCycle = memToSysCycle(respCycle);

    // Respond read requests. Writes have been responded at schedule time.
    if (req->ev) {
        assert(waitForWriteAck || (!req->isWrite && req->ev != nullptr));
        respondAccEvent(req->ev, sysRespCycle);
        req->ev = nullptr;
    }

    // Stats.
    uint32_t delay = sysRespCycle - req->startCycle + controllerSysDelay;
    if (req->isWrite) {
        profWrites.inc();
        profTotalWrLat.inc(delay);
        wrLatencyHist.inc(std::min(NUMBINS-1, delay/BINSIZE), 1);
    } else {
        profReads.inc();
        profTotalRdLat.inc(delay);
        rdLatencyHist.inc(std::min(NUMBINS-1, delay/BINSIZE), 1);
    }

    delete req;

    auto nextTickCycle = be->getTickCycleLowerBound();
    assert(nextTickCycle > memCycle);
    return nextTickCycle;
}

