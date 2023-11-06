#include <unordered_map>
#include <cstdint>
#include <list>
#include "memory_hierarchy.h"
#include "log.h"
#include "debug_output.h"
#include "load_balancing/limited_address_remap.h"
#include "zsim.h"
#include "comm_support/comm_module_manager.h"

using namespace pimbridge;

void LimitedAddressRemapTable::setChildRemap(Address lbPageAddr, int dst) {
    DEBUG_SCHED_META_O("%u-%u set childRemap: addr: %lu, val: %d", level, commId, lbPageAddr, dst);
    if (dst == -1) {
        childRemapTable.erase(lbPageAddr);
        this->lruList[getBucket(lbPageAddr)].remove(lbPageAddr);
    } else {
        childRemapTable[lbPageAddr] = dst;
        uint32_t bucketId = getBucket(lbPageAddr);
        this->updateLru(lbPageAddr);
        if (this->lruList[bucketId].size() > bucketSize) {
            Address evictAddr = this->lruList[bucketId].back();
            DEBUG_ADDR_RETURN_O("%u-%u evict %lu", level, commId, evictAddr);
            this->lruList[bucketId].pop_back();
            zinfo->commModuleManager->returnReplacedAddr(evictAddr, this->level, this->commId);
            // We do not need to erase childMap here because it has been done in returnReplacedAddr()
        }
    }
}

