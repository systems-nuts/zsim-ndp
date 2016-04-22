#ifndef FINITE_QUEUE_H_
#define FINITE_QUEUE_H_

#include "galloc.h"
#include "intrusive_list.h"

/**
 * A queue structure with finite size.
 *
 * Use two lists to track the used and free entries. Entries are ordered in the arrival order.
 */

template<typename T>
class FiniteQueue {
    private:
        struct Entry : InListNode<Entry> {
            T elem;
        };
        InList<Entry> usedList; // FIFO
        InList<Entry> freeList; // LIFO (higher locality)

    public:
        void init(size_t size) {
            assert(usedList.empty() && freeList.empty());
            Entry* buf = gm_calloc<Entry>(size);
            for (size_t i = 0; i < size; i++) {
                new (&buf[i]) Entry();
                freeList.push_back(&buf[i]);
            }
        }

        inline bool empty() const { return usedList.empty(); }
        inline bool full() const { return freeList.empty(); }
        inline size_t size() const { return usedList.size(); }

        inline T* alloc() {
            assert(!full());
            Entry* n = freeList.back();
            freeList.pop_back();
            usedList.push_back(n);
            return &n->elem;
        }

        struct iterator {
            Entry* n;
            explicit inline iterator(Entry* _n) : n(_n) {}
            inline void inc() {n = n->next;}  // overloading prefix/postfix too messy
            inline T* operator*() const { return &(n->elem); }
            inline bool operator==(const iterator& it) const { return it.n == n; }
            inline bool operator!=(const iterator& it) const { return it.n != n; }
        };

        inline iterator begin() const { return iterator(usedList.front()); }
        inline iterator end() const { return iterator(nullptr); }

        inline void remove(iterator i) {
            assert(i.n);
            usedList.remove(i.n);
            freeList.push_back(i.n);
        }
};

#endif  // FINITE_QUEUE_H_
