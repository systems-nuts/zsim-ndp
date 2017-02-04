#ifndef MEM_CHANNEL_BACKEND_DDR_H_
#define MEM_CHANNEL_BACKEND_DDR_H_

#include "finite_queue.h"
#include "g_std/g_vector.h"
#include "g_std/g_string.h"
#include "interval_recorder.h"
#include "mem_channel_backend.h"
#include "pad.h"
#include "stats.h"

/**
 * DDR memory backend.
 */
class MemChannelBackendDDR : public MemChannelBackend {
    public:
        struct Timing {
            uint32_t BL;    // burst
            uint32_t CAS;   // RD/WR -> data
            uint32_t CCD;   // RD/WR -> RD/WR
            uint32_t CWL;   // WR -> data begin, i.e., CWD
            uint32_t RAS;   // ACT -> PRE
            uint32_t RCD;   // ACT -> RD/WR
            uint32_t RP;    // PRE -> ACT
            uint32_t RRD;   // ACT -> ACT same bank
            uint32_t RTP;   // RD -> PRE
            uint32_t WR;    // WR data end -> PRE
            uint32_t WTR;   // WR data end -> RD
            // RC = RAS + RP

            // NOTE(mgao): tAL
            // We do not consider AL because command bus is not modeled
            // currently, and thus there is no difference.

            uint32_t RFC;   // REF -> ACT
            uint32_t REFI;  // REF -> REF
            uint32_t RPab;  // all-bank PRE -> ACT

            uint32_t FAW;   // four-bank ACT window
            uint32_t RTRS;  // rank to rank

            // FIXME(mgao): Command bus congestion is not modeled, thus always
            // using the min delay tCMD between commands.
            uint32_t CMD;   // command bus occupancy

            uint32_t XP;    // power-down exit latency
            uint32_t CKE;   // min power-down period

            uint32_t rdBurstChannelOccupyOverhead;  // cycles
            uint32_t wrBurstChannelOccupyOverhead;  // cycles
        };

        struct Power {
            // VDD is in mV
            uint32_t VDD;
            // IDD is in uA
            uint32_t IDD0;
            uint32_t IDD2N;
            uint32_t IDD2P;
            uint32_t IDD3N;
            uint32_t IDD3P;
            uint32_t IDD4R;
            uint32_t IDD4W;
            uint32_t IDD5;
            // Channel wire energy is in fJ/bit
            // This general wire energy can cover I/O termination, TSV, etc.
            uint32_t channelWireFemtoJoulePerBit;
        };

    public:
        MemChannelBackendDDR(const g_string& _name, uint32_t ranksPerChannel, uint32_t banksPerRank,
                const char* _pagePolicy, uint32_t pageSizeBytes,
                uint32_t burstCount, uint32_t deviceIOBits, uint32_t channelWidthBits,
                uint32_t memFreqMHz, const Timing& _t, const Power& _p,
                const char* addrMapping, uint32_t _queueDepth, bool _deferWrites,
                uint32_t _maxRowHits, uint32_t _powerDownCycles);

        uint64_t enqueue(const Address& addr, const bool isWrite, uint64_t startCycle,
                uint64_t memCycle, MemChannelAccEvent* respEv);

        bool dequeue(uint64_t memCycle, MemChannelAccReq** req, uint64_t* minTickCycle);

        bool queueOverflow(const bool isWrite) const;

        bool queueEmpty(const bool isWrite) const;

        uint64_t process(const MemChannelAccReq* req);

        void periodicalProcess(uint64_t memCycle, uint32_t index);

        uint64_t getTickCycleLowerBound() const {
            // Return min burst cycle here.
            // For high-memory-load, this is likely to be the next tick (back-to-back channel transfer),
            // for low-memory-load, the useless tick doesn't dominate simulation time.
            return minBurstCycle;
        }

        uint32_t getMemFreqKHz() const {
            return freqKHz;
        }

        uint32_t getMinLatency(const bool isWrite) const {
            return isWrite ? 0 : t.CAS + getBL(isWrite);
        }

        uint32_t getPeriodicalEventCount() const { return 2; }

        uint64_t getPeriodicalInterval(uint32_t index) const {
            // Event index:
            // 0: refresh a single rank.
            // 1: update background energy.
            if (index == 0) return t.REFI / rankCount;
            else if (index == 1) return 10000;
            else panic("Invalid periodical event index %u.", index);
        }

        void initStats(AggregateStat* parentStat);

    protected:
        struct DDRAddrMap {
            uint32_t rank;
            uint32_t bank;
            uint64_t row;
            uint32_t colH;
        };

        enum struct DDRPagePolicy {
            CLOSE,
            OPEN,
            RELAXED_CLOSE,
        };

        struct DDRAccReq : MemChannelAccReq, InListNode<DDRAccReq> {
            DDRAddrMap loc;

            // Sequence number used to throttle max number of row hits.
            uint32_t rowHitSeq;

            inline bool hasHighestPriority() const { return !this->prev; }
        };

        /* Efficiently track the activation window by a circular buffer.
         * Fast lookup, (relatively) slow insert. */
        class DDRActWindow : public GlobAlloc {
            private:
                // buf is in increasing order, idx is the logical oldest record.
                g_vector<uint64_t> buf;
                uint32_t idx;

            public:
                explicit DDRActWindow(uint32_t size) {
                    buf.resize(size);
                    for (uint32_t i = 0; i < size; i++) buf[i] = 0;
                    idx = 0;
                }

                inline uint64_t minACTCycle() const {
                    return buf[idx];
                }

                inline void addACT(uint64_t actCycle) {
                    assert(buf[idx] <= actCycle); // o/w we have violated the act window.

                    // Replace the oldest record by the new one and keep ordering.
                    uint32_t cur = idx;
                    while (buf[dec(cur)] > actCycle) {
                        buf[cur] = buf[dec(cur)];
                        cur = dec(cur);
                        if (cur == idx) break;
                    }
                    buf[cur] = actCycle;

                    // Move idx to the current oldest record (the one after the previous one).
                    idx = inc(idx);
                }

                // Use glob mem
                using GlobAlloc::operator new;
                using GlobAlloc::operator delete;

            private:
                inline uint32_t inc(uint32_t i) const { return (i < buf.size()-1)? i+1 : 0; }
                inline uint32_t dec(uint32_t i) const { return i? i-1 : buf.size()-1; }
        };

        // Track the state of a rank, including power-down.
        struct RankState : public GlobAlloc {
            // The last activity in the rank, after which we start to count powerDownCycles.
            uint64_t lastActivityCycle;
            // The last power-up cycle. All commands must be issued after power-up penalty after it.
            uint64_t lastPowerUpCycle;
            // The last power-down cycle.
            uint64_t lastPowerDownCycle;

            // The last cycle before which the background energy of the rank has been updated.
            uint64_t lastEnergyBKGDUpdateCycle;

            // Record active intervals of the rank (i.e., >= 1 banks are active).
            IntervalRecorder activeIntRec;

            // Last ACT cycle across all banks.
            uint64_t lastACTCycle;
            // Last RD/WR cycle across all banks.
            uint64_t lastRWCycle;
            // Last burst cycle across all banks.
            uint64_t lastBurstCycle;

            DDRActWindow actWindow4;

            RankState()
                : lastActivityCycle(0), lastPowerUpCycle(0), lastPowerDownCycle(0), lastEnergyBKGDUpdateCycle(0), activeIntRec(),
                  lastACTCycle(0), lastRWCycle(0), lastBurstCycle(0), actWindow4(4)
            {}
        };

        struct Bank {
            // Bank state and open row.
            bool open;
            uint64_t row;

            // Last PRE cycle for closed bank, or min cycle to issue PRE for open bank.
            uint64_t minPRECycle;
            // Last ACT cycle.
            uint64_t lastACTCycle;
            // Last RD/WR cycle.
            uint64_t lastRWCycle;

            RankState* rankState;

            // Sequence number for the last bank row hit.
            uint32_t rowHitSeq;

            explicit Bank(RankState* rs)
                : open(false), row(0), minPRECycle(0), lastACTCycle(0), lastRWCycle(0), rankState(rs) {}

            void recordPRE(uint64_t preCycle) {
                assert(open);
                open = false;
                minPRECycle = preCycle;
                rankState->activeIntRec.addInterval(lastACTCycle, preCycle);
            }

            void recordACT(uint64_t actCycle, uint64_t rowIdx) {
                assert(!open);
                open = true;
                row = rowIdx;
                lastACTCycle = actCycle;
                rankState->lastACTCycle = std::max(rankState->lastACTCycle, actCycle);
                rankState->actWindow4.addACT(actCycle);
            }

            void recordRW(uint64_t rwCycle) {
                assert(open);
                lastRWCycle = rwCycle;
                rankState->lastRWCycle = std::max(rankState->lastRWCycle, rwCycle);
            }
        };

    protected:
        // Handle a request and return the estimated tick cycle.
        // If update is true, then it is a real issued access and the states
        // will be updated. Otherwise no changes are made to the states.
        uint64_t requestHandler(const DDRAccReq* req, bool update = false);

        void refresh(uint64_t memCycle);

        void adjustPowerState(uint64_t memCycle, uint32_t rankIdx, uint32_t bankIdx, bool powerUp);

        virtual void assignPriority(DDRAccReq* req);
        virtual void cancelPriority(DDRAccReq* req);

        // Timing helper functions.
        uint64_t calcACTCycle(const Bank& bank, uint64_t schedCycle, uint64_t preCycle) const;
        uint64_t calcRWCycle(const Bank& bank, uint64_t schedCycle, uint64_t actCycle, bool isWrite, uint32_t rankIdx) const;
        uint64_t calcBurstCycle(const Bank& bank, uint64_t rwCycle, bool isWrite) const;
        uint64_t updatePRECycle(Bank& bank, uint64_t rwCycle, bool isWrite);

        uint32_t getBL(bool isWrite) const {
            return t.BL + (isWrite ? t.wrBurstChannelOccupyOverhead : t.rdBurstChannelOccupyOverhead);
        }

        // Energy helper functions.
        void updateEnergyACTPRE();
        void updateEnergyRDWR(bool isWrite);
        void updateEnergyREF();
        void updateEnergyBKGD(uint32_t cycles, bool powerDown, bool active);

    protected:
        const g_string name;

        const uint32_t rankCount;
        const uint32_t bankCount;

        const uint32_t pageSize;  // in Bytes
        const uint32_t burstSize; // in bits, deviceIO * # burst
        const uint32_t devicesPerRank;

        const uint32_t freqKHz;
        const Timing t;
        const Power p;

        const uint32_t powerDownCycles;

        DDRPagePolicy pagePolicy;

        g_vector<Bank> banks;

        // Address mapping scheme.

        uint32_t rankShift, bankShift, rowShift, colHShift;
        uint32_t rankMask, bankMask, rowMask, colHMask;

        // Address mapping from line address to DDR rank/bank/row/col.
        inline DDRAddrMap mapAddress(const Address& addr) {
            DDRAddrMap loc;
            loc.rank = (addr >> rankShift) & rankMask;
            loc.bank = (addr >> bankShift) & bankMask;
            loc.row = (addr >> rowShift) & rowMask;
            loc.colH = (addr >> colHShift) & colHMask;
            return loc;
        }

        // Refresh.

        uint32_t nextRankToRefresh;

        // Schedule and issue.

        // Request queue.
        const uint32_t queueDepth;
        bool deferWrites;
        FiniteQueue<DDRAccReq> reqQueueRd, reqQueueWr;

        inline const FiniteQueue<DDRAccReq>& reqQueue(const bool isWrite) const {
            return (deferWrites && isWrite) ? reqQueueWr : reqQueueRd;
        }
        inline FiniteQueue<DDRAccReq>& reqQueue(const bool isWrite) {
            return (deferWrites && isWrite) ? reqQueueWr : reqQueueRd;
        }

        // Priority list, per bank.
        g_vector<InList<DDRAccReq>> prioListsRd;
        g_vector<InList<DDRAccReq>> prioListsWr;

        inline const g_vector<InList<DDRAccReq>>& prioLists(const bool isWrite) const {
            return isWrite ? prioListsWr : prioListsRd;
        }
        inline g_vector<InList<DDRAccReq>>& prioLists(const bool isWrite) {
            return isWrite ? prioListsWr : prioListsRd;
        }

        // Max throttle of continuous row hits in a bank.
        uint32_t maxRowHits;

        // The issue mode. Decides how to issue the next access.
        enum struct IssueMode {
            RD_QUEUE,
            WR_QUEUE,
            UNKNOWN
        };
        IssueMode issueMode;
        // Mininum begin cycle for the next data burst transfer.
        // Also the end cycle for the last data burst transfer (1 cycle after the last transfer).
        uint64_t minBurstCycle;
        // Last request type.
        bool lastIsWrite;
        // Last request's rank. Used to track rank switch.
        uint32_t lastRankIdx;

        PAD();

        // Stats.
        Counter profACT, profPRE, profRD, profWR, profREF;

        Counter profEnergyACTPRE, profEnergyRDWR, profEnergyREF, profEnergyBKGD, profEnergyWIRE;
};

#endif  // MEM_CHANNEL_BACKEND_DDR_H_
