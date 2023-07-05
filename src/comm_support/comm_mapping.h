#pragma once
#include <cstdint>
#include <vector>

namespace pimbridge {

class CommMapping {
private:
    std::vector<std::vector<uint32_t>> mapping;
public:
    CommMapping(uint32_t numLevel, uint32_t numBank) {
        mapping.resize(numLevel);
        for (uint32_t i = 0; i < numLevel; ++i) {
            mapping[i].resize(numBank);
        }
    }
    void setMapping(uint32_t level, uint32_t bankBegin, uint32_t bankEnd, uint32_t commId) {
        for (uint32_t i = bankBegin; i < bankEnd; ++i) {
            mapping[level][i] = commId;
        }
    }
    inline uint32_t getCommId(uint32_t level, uint32_t bankId) {
        return mapping[level][bankId];
    }
};

}