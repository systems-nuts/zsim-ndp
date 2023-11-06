# pragma once
#include <unordered_map>
#include <cstdint>
#include <list>
#include "memory_hierarchy.h"
#include "log.h"
#include "debug_output.h"
#include "load_balancing/address_remap.h"

namespace pimbridge {

class LimitedAddressRemapTable : public AddressRemapTable {
private:
    uint32_t numBucket;
    uint32_t bucketSize;
    std::vector<std::list<Address>> lruList;
public:
    LimitedAddressRemapTable(uint32_t _level, uint32_t _commId, 
        uint32_t _numBucket, uint32_t _bucketSize) 
        : AddressRemapTable(_level, _commId), 
          numBucket(_numBucket), bucketSize(_bucketSize) {
        this->lruList.resize(numBucket);
    }
    void setChildRemap(Address lbPageAddr, int dst) override;
    void updateLru(Address lbPageAddr) override {
        this->lruList[getBucket(lbPageAddr)].remove(lbPageAddr);
        this->lruList[getBucket(lbPageAddr)].push_front(lbPageAddr);
    }
private:
    uint32_t getBucket(Address addr) {
        return (addr % numBucket);
    }
};


}