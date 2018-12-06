#include <bitset>
#include "numa_map.h"
#include "constants.h"
#include "g_std/g_unordered_map.h"
#include "intrusive_list.h"
#include "locks.h"
#include "scheduler.h"
#include "zsim.h"  // for getCid

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

/* Thread-safe, bucket-based page-to-node map. */
class PageMap : public GlobAlloc {
    public:
        PageMap() {
            futex_init(&futex);
        }

        bool isPresent(const Address pageAddr) {
            auto chunk = findChunk(pageAddr);
            return chunk && chunk->isPresent(pageAddr);
        }

        uint32_t get(const Address pageAddr) {
            auto chunk = findChunk(pageAddr);
            return chunk ? chunk->lookup(pageAddr) : NUMAMap::INVALID_NODE;
        }

        size_t add(Address pageAddr, size_t pageCount, const uint32_t node) {
            if (pageCount == 0) return 0;
            if (node == NUMAMap::INVALID_NODE) return pageCount;
            DEBUG("[PageMap] add %lx (%lu) -> %u", pageAddr, pageCount, node);
            size_t ignoredCount = 0;
            while (pageCount > 0) {
                Address nextPageAddr = nextChunkPageAddr(pageAddr);
                size_t cnt = std::min(pageCount, nextPageAddr - pageAddr);
                ignoredCount += findChunk(pageAddr, true)->add(pageAddr, cnt, node);
                pageCount -= cnt;
                pageAddr = nextPageAddr;
            }
            return ignoredCount;
        }

        void remove(Address pageAddr, size_t pageCount) {
            if (pageCount == 0) return;
            DEBUG("[PageMap] remove %lx (%lu)", pageAddr, pageCount);
            while (pageCount > 0) {
                Address nextPageAddr = nextChunkPageAddr(pageAddr);
                size_t cnt = std::min(pageCount, nextPageAddr - pageAddr);
                auto chunk = findChunk(pageAddr);
                if (chunk) chunk->remove(pageAddr, cnt);
                pageCount -= cnt;
                pageAddr = nextPageAddr;
            }
        }

    private:
        static constexpr uint32_t CHUNK_BITS = 16;  // 2^16 pages, i.e., 256 MB

        struct PageRange : InListNode<PageRange> {
            Address pageAddrBegin;
            Address pageAddrEnd;
            uint32_t node;

            // We need to keep the removed pages around. The remove happens at unmap time, but the data may not be
            // evicted from caches until later. We still need the map for writeback.
            // Pages marked as removed will be silently overwritten by newly added pages to the same place.
            bool removed;

            PageRange(const Address _pageAddr, const size_t _pageCount, const uint32_t _node, bool _removed = false)
                : pageAddrBegin(_pageAddr), pageAddrEnd(_pageAddr + _pageCount), node(_node), removed(_removed) {}

            inline bool contains(const Address pageAddr) const {
                return (pageAddr >= pageAddrBegin) && (pageAddr < pageAddrEnd);
            }

            inline size_t count() const {
                return pageAddrEnd - pageAddrBegin;
            }

            inline bool tryMergeWith(const PageRange& other) {
                if (node == other.node && removed == other.removed
                        && (pageAddrEnd >= other.pageAddrBegin && other.pageAddrEnd >= pageAddrBegin)) {
                    auto cnt = count() + other.count();
                    pageAddrBegin = std::min(pageAddrBegin, other.pageAddrBegin);
                    pageAddrEnd = std::max(pageAddrEnd, other.pageAddrEnd);
                    assert(count() <= cnt);
                    return true;
                }
                return false;
            }
        };

        class PageChunk {
        private:
            static constexpr uint64_t CHUNK_SIZE = 1 << PageMap::CHUNK_BITS;
            static constexpr Address CHUNK_MASK = (CHUNK_SIZE - 1);

            InList<PageRange> chunk;
            std::bitset<CHUNK_SIZE> pmap;
            lock_t futex;

        public:
            PageChunk() {
                futex_init(&futex);
            }

            bool isPresent(Address pageAddr) {
                // Check bitmap.
                futex_lock(&futex);
                bool p = pmap[pageAddr & CHUNK_MASK];
                futex_unlock(&futex);
                return p;
            }

            uint32_t lookup(Address pageAddr) {
                // Look up in chunk to get the node for the page.
                uint32_t node = NUMAMap::INVALID_NODE;
                futex_lock(&futex);
                if (pmap[pageAddr & CHUNK_MASK]) {
                    auto pr = chunk.front();
                    while (pr != nullptr) {
                        if (pr->contains(pageAddr)) {
                            node = pr->node;
                            break;
                        } else if (pr->pageAddrBegin > pageAddr) {
                            break;
                        }
                        pr = pr->next;
                    }
                }
                futex_unlock(&futex);
                return node;
            }

            size_t add(Address pageAddr, size_t pageCount, const uint32_t node) {
                // Add to chunk and keep the order, and merge if necessary.
                // Return number of pages that already exist and thus are ignored.
                size_t ignoredCount = 0;
                auto newpr = new PageRange(pageAddr, pageCount, node);
                futex_lock(&futex);
                for (size_t i = 0; i < pageCount; i++) {
                    pmap.set((pageAddr & CHUNK_MASK) + i);
                }
                auto pr = chunk.front();
                while (pr && newpr) {
                    if (newpr->pageAddrEnd < pr->pageAddrBegin) {
                        // No overlap with following ones, insert as new entry.
                        safeInsertBefore(pr, newpr);
                        newpr = nullptr;
                    } else if (newpr->pageAddrBegin > pr->pageAddrEnd) {
                        // No overlap but after the current one, do nothing.
                        pr = pr->next;
                    } else if (pr->removed) {
                        // Overlaps with current one which has been removed.
                        // Overwrite the overlapped part.

                        // Split the current range.
                        PageRange* before = nullptr;
                        PageRange* after = nullptr;
                        split(pr, newpr, &before, nullptr, &after);
                        assert((before ? before->count() : 0) + (after ? after->count() : 0) <= pr->count());
                        // Remove it.
                        auto q = pr;
                        pr = pr->next;
                        chunk.remove(q);
                        delete q;

                        // Add back non-overlap range before the new range.
                        if (before) {
                            safeInsertBefore(pr, before);
                        }

                        // If there is non-overlap range after the new range ...
                        if (after) {
                            // Insert the new range.
                            safeInsertBefore(pr, newpr);
                            newpr = nullptr;

                            // Add back non-overlap range after the new range.
                            safeInsertBefore(pr, after);
                        }
                        // ... otherwise, the current removed range is completely handled.
                        // We leave the new range unchanged to compare with the next one.
                    } else if (node == pr->node) {
                        // Overlaps with current one and the same node.
                        // Remove the current one and merge into the new range.
                        assert(newpr->tryMergeWith(*pr));
                        auto q = pr;
                        pr = pr->next;
                        chunk.remove(q);
                        delete q;
                    } else {
                        // Overlaps with current one but different nodes.
                        // Insert the non-overlapped part.

                        // Split the new range.
                        PageRange* before = nullptr;
                        PageRange* after = nullptr;
                        split(newpr, pr, &before, nullptr, &after);
                        assert((before ? before->count() : 0) + (after ? after->count() : 0) <= newpr->count());

                        // Overlap range is ignored.
                        ignoredCount += newpr->count() - (before ? before->count() : 0) - (after ? after->count() : 0);

                        // Insert non-overlap range before the current one.
                        if (before) {
                            safeInsertBefore(pr, before);
                        }

                        // Use non-overlap range after the current one as the new one.
                        newpr = after;

                        pr = pr->next;
                    }
                }
                // Push to the end.
                if (newpr) chunk.push_back(newpr);
                verify();
                futex_unlock(&futex);
                return ignoredCount;
            }

            void remove(Address pageAddr, size_t pageCount) {
                // Remove lazily from chunk, and split if necessary.
                auto e = PageRange(pageAddr, pageCount, NUMAMap::INVALID_NODE, true);
                auto rempr = &e;
                futex_lock(&futex);
                for (size_t i = 0; i < pageCount; i++) {
                    pmap.set((pageAddr & CHUNK_MASK) + i, false);
                }
                auto pr = chunk.front();
                while (pr) {
                    if (rempr->pageAddrEnd < pr->pageAddrBegin) {
                        // No overlap with following ones, return.
                        break;
                    } else if (rempr->pageAddrBegin > pr->pageAddrEnd) {
                        // No overlap but after the current one, continue.
                        pr = pr->next;
                    } else if (!pr->removed) {
                        // Overlaps with current one which is not removed.
                        // Mark the overlapped part as removed.

                        // Split the current range.
                        PageRange* before = nullptr;
                        PageRange* overlap = nullptr;
                        PageRange* after = nullptr;
                        split(pr, rempr, &before, &overlap, &after);
                        assert((before ? before->count() : 0)
                                + (after ? after->count() : 0)
                                + (overlap ? overlap->count() : 0) <= pr->count());
                        // Remove it.
                        auto q = pr;
                        pr = pr->next;
                        chunk.remove(q);
                        delete q;

                        // Add back non-overlap range before the removing range.
                        if (before) {
                            safeInsertBefore(pr, before);
                        }

                        // Add back overlap range as removed.
                        if (overlap) {
                            assert(!overlap->removed);
                            overlap->removed = true;

                            // Try merge with removed neighbors.
                            auto prev = pr ? pr->prev : chunk.back();
                            if (prev && overlap->tryMergeWith(*prev)) {
                                auto q = prev;
                                chunk.remove(q);
                                delete q;
                            }
                            if (pr && overlap->tryMergeWith(*pr)) {
                                auto q = pr;
                                pr = pr->next;
                                chunk.remove(q);
                                delete q;
                            }
                            safeInsertBefore(pr, overlap);
                        }

                        // Add back non-overlap range after the removing range.
                        if (after) {
                            safeInsertBefore(pr, after);
                        }
                    } else {
                        // Overlaps with current one which has been removed, do nothing.
                        pr = pr->next;
                    }
                }
                verify();
                futex_unlock(&futex);
            }

            void verify() const {
#if 0
                auto pr = chunk.front();
                uint64_t lastPageAddrEnd = 0;
                uint64_t lastNode = NUMAMap::INVALID_NODE;
                while (pr != nullptr) {
                    assert(pr->pageAddrBegin < pr->pageAddrEnd);
                    assert(pr->node != NUMAMap::INVALID_NODE);
                    if (pr->node == lastNode) {
                        assert_msg(pr->pageAddrBegin > lastPageAddrEnd, "page ranges overlap");
                    } else {
                        assert_msg(pr->pageAddrBegin >= lastPageAddrEnd, "page ranges overlap");
                    }
                    pr = pr->next;
                }
#endif
            }

        private:
            inline void safeInsertBefore(PageRange* next, PageRange* e) {
                if (next == nullptr) {
                    chunk.push_back(e);
                    return;
                }
                auto prev = next->prev;
                if (prev == nullptr) {
                    chunk.push_front(e);
                    return;
                }
                assert(prev->pageAddrEnd < e->pageAddrBegin || prev->node != e->node || prev->removed != e->removed);
                assert(!prev->next || e->pageAddrEnd < prev->next->pageAddrBegin || prev->next->node != e->node || prev->next->removed != e->removed);
                chunk.insertAfter(prev, e);
            }

            inline void split(const PageRange* orig, const PageRange* splitter,
                    PageRange** before, PageRange** overlap, PageRange** after) {
                auto makeRange = [orig](Address begin, Address end) -> PageRange* {
                    if (begin < end) return new PageRange(begin, end - begin, orig->node, orig->removed);
                    return nullptr;
                };
                if (before != nullptr) *before = makeRange(orig->pageAddrBegin, splitter->pageAddrBegin);
                if (overlap != nullptr) *overlap = makeRange(std::max(orig->pageAddrBegin, splitter->pageAddrBegin), std::min(orig->pageAddrEnd, splitter->pageAddrEnd));
                if (after != nullptr) *after = makeRange(splitter->pageAddrEnd, orig->pageAddrEnd);
            }
        };

        g_unordered_map<uint64_t, PageChunk> pageMaps;

        lock_t futex;

    private:
        PageChunk* findChunk(const Address pageAddr, bool insert = false) {
            uint64_t chunkIdx = pageAddr >> CHUNK_BITS;
            PageChunk* chunk = nullptr;
            futex_lock(&futex);
            if (insert) {
                chunk = &pageMaps[chunkIdx];
            } else {
                auto it = pageMaps.find(chunkIdx);
                if (it != pageMaps.end()) {
                    chunk = &it->second;
                }
            }
            futex_unlock(&futex);
            return chunk;
        }

        inline static Address nextChunkPageAddr(const Address pageAddr) {
            uint64_t chunkIdx = pageAddr >> CHUNK_BITS;
            return (chunkIdx + 1) << CHUNK_BITS;
        }
};


constexpr uint32_t NUMAMap::INVALID_NODE;

NUMAMap::NUMAMap(const char* _patchRoot, const uint32_t numCores)
    : patchRoot(_patchRoot == nullptr ? "" : _patchRoot),
      coreNodeMap(numCores, INVALID_NODE), pageNodeMap(new PageMap())
{
    // Use patched root to figure out NUMA core map.
    if (patchRoot.empty()) panic("NUMA needs to patch the root path in the main process!");
    uint32_t node = 0;
    while (parseBitmap(node) == 0 && node < 1024 /*avoid inf loop*/) { node++; }
    maxNode = node - 1;
    for (uint32_t cid = 0; cid < numCores; cid++) {
        if (coreNodeMap[cid] == INVALID_NODE) {
            warn("Core %u has no associated NUMA node", cid);
        }
    }
}

uint32_t NUMAMap::getNodeOfPage(const Address pageAddr) {
    auto node = pageNodeMap->get(pageAddr);
    assert_msg(node != INVALID_NODE, "Page addr %lx has not been allocated!", pageAddr);
    return node;
}

void NUMAMap::allocateFromCore(const Address addr, const uint32_t cid) {
    auto pageAddr = getPageAddress(addr);
    if (unlikely(!pageNodeMap->isPresent(pageAddr))) {
        assert(cid < zinfo->numCores);
        uint32_t tid = zinfo->sched->getScheduledTid(cid);
        assert_msg(tid != -1u, "Core %u has no thread running! Who is allocating the line?", cid);
        assert(addPagesThreadPolicy(pageAddr, 1, tid, cid) == 0);
    }
}

size_t NUMAMap::addPagesToNode(const Address pageAddr, const size_t pageCount, const uint32_t node) {
    return pageNodeMap->add(pageAddr, pageCount, node);
}

void NUMAMap::removePages(const Address pageAddr, const size_t pageCount) {
    pageNodeMap->remove(pageAddr, pageCount);
}

size_t NUMAMap::addPagesThreadPolicy(const Address pageAddr, const size_t pageCount, const uint32_t tid, const uint32_t cid, NUMAPolicy* policy) {
    if (!policy) {
        // Use the policy of the thread.
        policy = &threadPolicy[tid];
    }
    const auto& mode = policy->getMode();

    size_t ignoredCount = 0;
    bool success = false;

    // See Linux doc set_mempolicy(2).
    if (mode == MPOL_DEFAULT
#ifdef MPOL_LOCAL
            || mode == MPOL_LOCAL
#endif  // MPOL_LOCAL
       ) {
        // Local allocation.
        success = tryAddPagesLocal(pageAddr, pageCount, getNodeOfCore(cid), false, ignoredCount);
    } else if (mode == MPOL_PREFERRED) {
        // Preferred node allocation.
        // The preferred node is the first node in nodemask.
        uint32_t node = 0;
        while (node <= maxNode && !policy->isAllowed(node)) node++;
        if (node > maxNode) {
            // Empty nodemask. Fall back to default policy.
            node = getNodeOfCore(cid);
        }
        success = tryAddPagesLocal(pageAddr, pageCount, node, false, ignoredCount);
    } else if (mode == MPOL_BIND) {
        // Strict bind allocation.
        for (uint32_t node = 0; node <= maxNode; node++) {
            if (!policy->isAllowed(node)) continue;
            success = tryAddPagesLocal(pageAddr, pageCount, node, true, ignoredCount);
            if (success) break;
        }
    } else if (mode == MPOL_INTERLEAVE) {
        // Interleaving allocation.
        // Interleave across the allowed nodes.
        success = tryAddPagesInterleaved(pageAddr, pageCount, policy, ignoredCount);
    } else {
        panic("Invalid NUMA policy mode %d", mode);
    }

    if (!success) panic("NUMA allocation fails. Thread %u, mode %d, page count %lu", tid, mode, pageCount);
    assert(ignoredCount <= pageCount);
    return ignoredCount;
}

int NUMAMap::parseBitmap(const uint32_t node) {
    // Open cpumap file.
    size_t fnLen = patchRoot.size() + 128;
    char* fname = new char[fnLen];
    snprintf(fname, fnLen, "%s/sys/devices/system/node/node%u/cpumap", patchRoot.c_str(), node);
    FILE* f = fopen(fname, "r");
    delete[] fname;
    if (!f) return -1;

    // Read bitmap string.
    char* line = nullptr;
    size_t len = 0;
    int ret = getline(&line, &len, f);
    fclose(f);
    if (ret < 0) return -1;

    // Parse bitmap string.
    char* p = strchr(line, '\n');
    if (!p) {
        free(line);
        return -1;
    }
    for (uint32_t i = 0; p > line; i++) {
        char* oldp = p;
        if (*p == ',') --p;
        while (p > line && *p != ',') --p;
        if (*p == ',') p++;
        // Now p points to the first number, oldp points to the char after the last number.
        // There are 8 chars between p and oldp, corresponding to a 32-bit mask.
        assert(oldp - p == 8);
        char* endp;
        uint32_t mask = strtoul(p, &endp, 16);
        if (endp != oldp) {
            free(line);
            return -1;
        }
        p--;

        // Update core-to-node map.
        if (!mask) continue;
        for (uint32_t bit = 0; bit < 32; bit++) {
            bool isset = (mask >> bit) & 0x1;
            if (isset) {
                uint32_t cid = i * 32 + bit;
                assert_msg(cid < coreNodeMap.size(),
                        "There is more cores in patched root (>= %u) than in system config (%lu)",
                        cid, coreNodeMap.size());
                assert_msg(coreNodeMap[cid] == INVALID_NODE,
                        "Core %u belongs to multiple NUMA node %u and %u",
                        cid, coreNodeMap[cid], node);
                coreNodeMap[cid] = node;
            }
        }
    }

    free(line);
    return 0;
}

bool NUMAMap::tryAddPagesLocal(const Address pageAddr, const size_t pageCount, uint32_t node, bool strict, size_t& ignoredCount) {
    if (node == INVALID_NODE) {
        // This core does not belong to any node, i.e., memory-less node.
        // Interleave the allocation across all nodes.
        assert_msg(!strict, "Local allocation cannot be strict for memory-less node.");
        NUMAPolicy ip(MPOL_INTERLEAVE, g_vector<bool>(maxNode + 1, true));
        return tryAddPagesInterleaved(pageAddr, pageCount, &ip, ignoredCount);
    }

    if (strict) {
        // TODO: fail if not enough memory.
        ignoredCount += addPagesToNode(pageAddr, pageCount, node);
        return true;
    }

    // Not strict, also try nearby nodes.
    // We try the next node one by one.
    for (uint32_t t = 0; t <= maxNode; t++) {
        if (tryAddPagesLocal(pageAddr, pageCount, node, true, ignoredCount)) return true;
        node = (node + 1) % (maxNode + 1);
    }
    return false;
}

bool NUMAMap::tryAddPagesInterleaved(const Address pageAddr, const size_t pageCount, NUMAPolicy* policy, size_t& ignoredCount) {
    assert(policy && policy->getMode() == MPOL_INTERLEAVE);
    size_t igcnt = 0;
    for (size_t pa = pageAddr; pa < pageAddr + pageCount; pa++) {
        bool success = false;
        for (size_t t = 0; t <= maxNode && !success; t++) {
            success = tryAddPagesLocal(pa, 1, policy->updateNext(), true, igcnt);
        }
        if (!success) return false;
    }
    assert(igcnt <= pageCount);
    ignoredCount += igcnt;
    return true;
}

