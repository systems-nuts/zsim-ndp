#ifndef MEM_CHANNEL_BACKEND_DDR_H_
#define MEM_CHANNEL_BACKEND_DDR_H_

#include "finite_queue.h"
#include "g_std/g_vector.h"
#include "g_std/g_string.h"
#include "mem_channel_backend.h"

/**
 * DDR memory backend.
 */
class MemChannelBackendDDR : public MemChannelBackend {
    public:
        struct Timing {
            uint32_t BL;    // burst
            uint32_t CAS;   // RD/WR -> data
            uint32_t CCD;   // RD/WR -> RD/WR
            uint32_t CWD;   // WR -> data begin
            uint32_t RAS;   // ACT -> PRE
            uint32_t RCD;   // ACT -> RD/WR
            uint32_t RP;    // PRE -> ACT
            uint32_t RRD;   // ACT -> ACT same bank
            uint32_t RTP;   // RD -> PRE
            uint32_t WR;    // WR data end -> PRE
            uint32_t WTR;   // WR data end -> RD
            // RC = RAS + RP

            uint32_t RFC;   // REF -> ACT
            uint32_t REFI;  // REF -> REF
            uint32_t RPab;  // all-bank PRE -> ACT

            uint32_t FAW;   // four-bank ACT window
            uint32_t RTRS;  // rank to rank

            uint32_t CMD;   // command bus occupancy
            uint32_t XP;    // power-down exit latency
        };

    public:
        MemChannelBackendDDR(const g_string& _name, uint32_t ranksPerChannel, uint32_t banksPerRank,
                const char* _pagePolicy, uint32_t pageSizeBytes,
                uint32_t burstCount, uint32_t deviceIOBits, uint32_t channelWidthBits,
                uint32_t memFreqMHz, const Timing& _t, const char* addrMapping, uint32_t _queueDepth,
                uint32_t _maxRowHits);

        uint64_t enqueue(const Address& addr, const bool isWrite, uint64_t startCycle,
                uint64_t memCycle, MemChannelAccEvent* respEv);

        bool dequeue(uint64_t memCycle, MemChannelAccReq** req, uint64_t* minTickCycle);

        bool queueOverflow(const bool isWrite) const;

        bool queueEmpty(const bool isWrite) const;

        uint64_t process(const MemChannelAccReq* req);

        void periodicalProcess(uint64_t memCycle);

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
            return isWrite ? 0 : t.CAS + t.BL;
        }

        uint64_t getPeriodicalInterval() const {
            return t.REFI;
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

            DDRActWindow* actWindow;

            // Sequence number for the last bank row hit.
            uint32_t rowHitSeq;

            explicit Bank(DDRActWindow* aw)
                : open(false), row(0), minPRECycle(0), lastACTCycle(0), lastRWCycle(0), actWindow(aw) {}

            void recordPRE(uint64_t preCycle) {
                assert(open);
                open = false;
                minPRECycle = preCycle;
            }

            void recordACT(uint64_t actCycle, uint64_t rowIdx) {
                assert(!open);
                open = true;
                row = rowIdx;
                lastACTCycle = actCycle;
                actWindow->addACT(actCycle);
            }

            void recordRW(uint64_t rwCycle) {
                assert(open);
                lastRWCycle = rwCycle;
            }
        };

    protected:
        // Handle a request and return the estimated tick cycle.
        // If update is true, then it is a real issued access and the states
        // will be updated. Otherwise no changes are made to the states.
        uint64_t requestHandler(const DDRAccReq* req, bool update = false);

        void refresh(uint64_t memCycle);

        virtual void assignPriority(DDRAccReq* req);
        virtual void cancelPriority(DDRAccReq* req);

        // Timing helper functions.
        uint64_t calcACTCycle(const Bank& bank, uint64_t schedCycle, uint64_t preCycle) const;
        uint64_t calcRWCycle(const Bank& bank, uint64_t schedCycle, uint64_t actCycle, bool isWrite, uint32_t rankIdx) const;
        uint64_t calcBurstCycle(const Bank& bank, uint64_t rwCycle, bool isWrite) const;
        uint64_t updatePRECycle(Bank& bank, uint64_t rwCycle, bool isWrite);

    protected:
        const g_string name;

        const uint32_t rankCount;
        const uint32_t bankCount;

        const uint32_t pageSize;  // in Bytes
        const uint32_t burstSize; // in bits, deviceIO * # burst
        const uint32_t devicesPerRank;

        const uint32_t freqKHz;
        const Timing t;

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

        // Schedule and issue.

        // Request queue.
        const uint32_t queueDepth;
        FiniteQueue<DDRAccReq> reqQueueRd, reqQueueWr;

        inline const FiniteQueue<DDRAccReq>& reqQueue(const bool isWrite) const {
            return isWrite ? reqQueueWr : reqQueueRd;
        }
        inline FiniteQueue<DDRAccReq>& reqQueue(const bool isWrite) {
            return isWrite ? reqQueueWr : reqQueueRd;
        }

        // Priority list, per bank.
        g_vector<InList<DDRAccReq>> prioLists;
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
};

#endif  // MEM_CHANNEL_BACKEND_DDR_H_
