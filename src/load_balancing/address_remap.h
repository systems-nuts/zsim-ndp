# pragma once
#include <unordered_map>
#include <cstdint>
#include "memory_hierarchy.h"
#include "log.h"
#include "debug_output.h"

namespace pimbridge {

// For maintaining data remapping for task scheduling. 
// Each CommModuleBase holds an AddressRemapTable, 
// But in BottomCommModule, childRemapTable should be empty, since it has no children.
class AddressRemapTable {
private:
    uint32_t level;
    uint32_t commId;
    // Used to maintain whether an address is lent out of this commModule
    // the key is page address. 
    std::unordered_map<Address, uint32_t> addrLend;
    // Used to maintain whether an address is borrowed into this commModule
    // the key is page address. 
    std::unordered_map<Address, uint32_t> addrBorrowMidState;
    // Used to record the remapping between child commModule.
    // the key is page address and the value is commId.
    std::unordered_map<Address, uint32_t> childRemapTable;

public:
    AddressRemapTable(uint32_t _level, uint32_t _commId) 
        : level(_level), commId(_commId) {}

    /* --- getters and setters --- */
    void setAddrLend(Address lbPageAddr, bool val) {
        DEBUG_SCHED_META_O("%u-%u set addr lend: addr: %lu, val: %u", level, commId, lbPageAddr, val);
        if (val) {
            addrLend[lbPageAddr] = 1;
        } else {
            addrLend.erase(lbPageAddr);
        }
    }
    bool getAddrLend(Address lbPageAddr) {
        return (addrLend.count(lbPageAddr) != 0);
    }
    void setAddrBorrowMidState(Address lbPageAddr, uint32_t id) {
        assert(this->level == 0);
        // assert(this->childRemapTable.count(lbPageAddr) == 0);
        if (this->childRemapTable.count(lbPageAddr) != 0) {
            return;
        }
        DEBUG_SCHED_META_O("%u-%u set mid state: addr: %lu, id: %u", 
            level, commId, lbPageAddr, id);
        if (id == 0) {
            // assert(addrBorrowMidState.count(lbPageAddr) == 0);
            addrBorrowMidState[lbPageAddr] = 0;
        } else {
            // assert_msg(addrBorrowMidState.count(lbPageAddr) != 0 &&
            //     addrBorrowMidState[lbPageAddr] == id - 1,
            //     "addr: %lu, id: %u, level: %u, commId: %u", 
            //     lbPageAddr, id, level, commId);
            addrBorrowMidState[lbPageAddr] = id;
        }
    }
    void eraseAddrBorrowMidState(Address lbPageAddr) {
        this->addrBorrowMidState.erase(lbPageAddr);
    }
    bool getAddrBorrowMidState(Address lbPageAddr) {
        return (addrBorrowMidState.count(lbPageAddr) != 0);
    }
    void setChildRemap(Address lbPageAddr, int dst) {
        DEBUG_SCHED_META_O("%u-%u set childRemap: addr: %lu, val: %d", level, commId, lbPageAddr, dst);
        if (dst == -1) {
            childRemapTable.erase(lbPageAddr);
        } else {
            childRemapTable[lbPageAddr] = dst;
        }
    }
    // -1 means lbPageAddr is not remapped.
    int getChildRemap(Address lbPageAddr) {
        if (childRemapTable.count(lbPageAddr) == 0) {
            return -1;
        } else {
            return childRemapTable[lbPageAddr];
        }
    }
};



}