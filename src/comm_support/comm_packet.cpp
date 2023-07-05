#include <cstdint>
#include "task_support/task.h"
#include "comm_support/comm_packet.h"
#include "zsim.h"
#include "numa_map.h"

namespace pimbridge {

Address TaskCommPacket::getAddr() {
    assert(this->task->hint->dataPtr != 0);
    return zinfo->numaMap->getLbPageAddress(this->task->hint->dataPtr);
}
}

