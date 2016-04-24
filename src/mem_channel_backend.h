#ifndef MEM_CHANNEL_BACKEND_H_
#define MEM_CHANNEL_BACKEND_H_

#include "intrusive_list.h"
#include "memory_hierarchy.h"

class MemChannelAccEvent;

// Access request record. Linked in a linked list based on the priority.
struct MemChannelAccReq : InListNode<MemChannelAccReq> {
};

class MemChannelBackend {
    public:
        virtual bool queueOverflow(const bool isWrite) const = 0;
};

#endif  // MEM_CHANNEL_BACKEND_H_
