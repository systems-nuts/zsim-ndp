# pragma once
#include <unordered_map>
#include <cstdint>
#include "memory_hierarchy.h"
#include "log.h"
#include "debug_output.h"
#include "load_balancing/address_remap.h"

/*
namespace pimbridge {

class NewAddressRemapTable : public AddressRemapTable {
protected:
    std::unordered_map<Address, int> childRemapTable;
public:
    NewAddressRemapTable(uint32_t _level, uint32_t _commId)
        : AddressRemapTable(_level, _commId) {}
    
    void setAddrBorrowMidState(Address lbPageAddr, uint32_t id) override {
        assert(this->level == 0);
        if (this->childRemapTable.count(lbPageAddr) != 0 && 
            this->childRemapTable[lbPageAddr] > 0) {
            return;
        }
        DEBUG_SCHED_META_O("%u-%u set mid state: addr: %lu, id: %u", 
            level, commId, lbPageAddr, id);
        childRemapTable[lbPageAddr] = -id;
    }
    void eraseAddrBorrowMidState(Address lbPageAddr) override {
        this->childRemapTable.erase(lbPageAddr);
    }
    bool getAddrBorrowMidState(Address lbPageAddr) override {
        return this->childRemapTable.count(lbPageAddr) != 0 &&
            this->childRemapTable[lbPageAddr] <= 0;
    }
    void setChildRemap(Address lbPageAddr, int dst) override {
        DEBUG_SCHED_META_O("%u-%u set childRemap: addr: %lu, val: %d", level, commId, lbPageAddr, dst);
        if (dst == -1) {
            childRemapTable.erase(lbPageAddr);
        } else {
            childRemapTable[lbPageAddr] = dst+1;
        }
    }
    // -1 means lbPageAddr is not remapped.
    int getChildRemap(Address lbPageAddr) override {
        if (childRemapTable.count(lbPageAddr) == 0) {
            return -1;
        } else {
            return childRemapTable[lbPageAddr]-1;
        }
    }


};

}*/