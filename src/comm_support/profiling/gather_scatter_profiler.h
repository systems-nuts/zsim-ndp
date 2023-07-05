#pragma once
#include "locks.h"
#include "config.h"

namespace pimbridge {

class GatherScatterProfiler {
private:
    class BwUtilEntry {
    public: 
        uint32_t sum;
        uint32_t transferSizes[4];
        uint32_t maxNumChild = 4;
        BwUtilEntry() : sum(0) {
            for (uint32_t i = 0; i < maxNumChild; ++i) {
                transferSizes[i] = 0;
            }
        }
        void recordTransfer(uint32_t idx, uint32_t size) {
            assert(idx < maxNumChild);
            this->transferSizes[idx] += size;
            this->sum += size;
        }
    };
    bool enableTrace;
    lock_t lock;
    std::string pathPrefix;
    // level, commModule, each operation
    std::vector<std::vector<std::vector<BwUtilEntry>>> bwUtil;
    std::vector<std::vector<std::vector<uint64_t>>> intervalLength;
public:
    GatherScatterProfiler(Config& config, const std::string& prefix);
    GatherScatterProfiler() : enableTrace(false) {}
    void initTransfer(uint32_t level, uint32_t commId);
    void record(uint32_t level, uint32_t commId, uint32_t childIdx, uint32_t size);
    void toFile();
private:
    void bwUtilToFile();
};


}