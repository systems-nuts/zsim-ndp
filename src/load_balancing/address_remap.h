# pragma once
#include <unordered_map>
#include <cstdint>
#include "memory_hierarchy.h"
#include "log.h"

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
    std::unordered_map<Address, uint32_t> addrBorrow;
    std::unordered_map<Address, uint32_t> addrBorrowMidState;
    // Used to record the remapping between child commModule.
    // the value in childRemapTable is always bankId, no matter what the level is, 
    // which is the same to the implementation of communication packet. 
    // the key is page address
    std::unordered_map<Address, uint32_t> childRemapTable;

public:
    AddressRemapTable(uint32_t _level, uint32_t _commId) 
        : level(_level), commId(_commId) {}

    /* --- getters and setters --- */
    void setAddrLend(Address pageAddr, bool val) {
        if (val) {
            addrLend[pageAddr] = 1;
        } else {
            assert(addrLend.count(pageAddr) != 0);
            this->addrBorrowMidState.erase(pageAddr);
            addrLend.erase(pageAddr);
        }
    }
    bool getAddrLend(Address pageAddr) {
        return addrLend.find(pageAddr) != addrLend.end();
    }
    void setAddrBorrow(Address pageAddr, bool val) {
        if (val) {
            this->addrBorrowMidState.erase(pageAddr);
            addrBorrow[pageAddr] = 1;
        } else {
            assert(addrBorrow.count(pageAddr) != 0);
            addrBorrow.erase(pageAddr);
        }
    }
    void setAddrBorrowMidState(Address pageAddr, uint32_t id) {
        assert(this->addrBorrow.count(pageAddr) == 0);
        if (id == 0) {
            assert(addrBorrowMidState.count(pageAddr) == 0);
            addrBorrowMidState.insert(std::make_pair(pageAddr, 0));
        } else {
            assert(getAddrBorrowMidState(pageAddr) && 
                addrBorrowMidState[pageAddr] == id - 1);
            addrBorrowMidState[pageAddr] = id;
        }
    }
    bool getAddrBorrowMidState(Address pageAddr) {
        return addrBorrowMidState.find(pageAddr) != addrBorrowMidState.end();
    }
    bool getAddrBorrow(Address pageAddr) {
        return addrBorrow.find(pageAddr) != addrBorrow.end();
    }
    void setChildRemap(Address pageAddr, uint32_t dst) {
        childRemapTable[pageAddr] = dst;
    }
    // -1 means pageAddr is not remapped.
    int getChildRemap(Address pageAddr) {
        if (childRemapTable.count(pageAddr) == 0) {
            return -1;
        } else {
            return childRemapTable[pageAddr];
        }
    }
};



}