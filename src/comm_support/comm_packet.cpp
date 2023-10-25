#include <cstdint>
#include "task_support/task.h"
#include "comm_support/comm_packet.h"
#include "zsim.h"
#include "numa_map.h"

namespace pimbridge {

TaskCommPacket::TaskCommPacket(uint32_t _timeStamp, uint64_t _readyCycle,
    uint32_t _fromLevel, uint32_t _fromCommId, uint32_t _toLevel, int _toCommId,
    task_support::TaskPtr _task, uint32_t _priority)
    : CommPacket(PacketType::Task, PacketType::Task,
                 _timeStamp, _readyCycle, _fromLevel, _fromCommId, 
                 _toLevel, _toCommId, _priority), 
      task(_task) {
    this->size = this->task->taskSize;
    this->signature = this->task->taskId;
    assert(this->task->hint->dataPtr != 0);
    this->addr = zinfo->numaMap->getLbPageAddress(this->task->hint->dataPtr);
}

}

