#include "numa_map.h"
#include "g_std/g_unordered_map.h"
#include "intrusive_list.h"
#include "locks.h"

/* Thread-safe, bucket-based page-to-node map. */
class PageMap : public GlobAlloc {
    public:
        PageMap() {
            futex_init(&futex);
        }

        uint32_t get(const Address pageAddr) {
            uint64_t chunkIdx = pageAddr >> chunkBits;
            uint32_t node = NUMAMap::INVALID_NODE;
            futex_lock(&futex);
            auto chunkIter = pageMaps.find(chunkIdx);
            if (chunkIter != pageMaps.end()) {
                node = chunkIter->second.lookup(pageAddr);
            }
            futex_unlock(&futex);
            return node;
        }

        size_t add(Address pageAddr, size_t pageCount, const uint32_t node) {
            if (pageCount == 0) return 0;
            if (node == NUMAMap::INVALID_NODE) return pageCount;
            size_t ignoredCount = 0;
            futex_lock(&futex);
            while (pageCount > 0) {
                uint64_t chunkIdx = pageAddr >> chunkBits;
                Address nextPageAddr = (chunkIdx + 1) << chunkBits;
                size_t chunkNumPages = std::min(pageCount, nextPageAddr - pageAddr);
                ignoredCount += pageMaps[chunkIdx].add(pageAddr, chunkNumPages, node);
                pageMaps[chunkIdx].verify(chunkIdx);
                pageCount -= chunkNumPages;
                pageAddr = nextPageAddr;
            }
            futex_unlock(&futex);
            return ignoredCount;
        }

        void remove(Address pageAddr, size_t pageCount) {
            if (pageCount == 0) return;
            futex_lock(&futex);
            while (pageCount > 0) {
                uint64_t chunkIdx = pageAddr >> chunkBits;
                Address nextPageAddr = (chunkIdx + 1) << chunkBits;
                size_t chunkNumPages = std::min(pageCount, nextPageAddr - pageAddr);
                auto chunkIter = pageMaps.find(chunkIdx);
                if (chunkIter != pageMaps.end()) {
                    chunkIter->second.remove(pageAddr, chunkNumPages);
                    chunkIter->second.verify(chunkIdx);
                }
                pageCount -= chunkNumPages;
                pageAddr = nextPageAddr;
            }
            futex_unlock(&futex);
        }

    private:
        struct PageRange : InListNode<PageRange> {
            const Address pageAddrBegin;
            const Address pageAddrEnd;
            const uint32_t node;

            PageRange(const Address _pageAddr, const size_t _pageCount, const uint32_t _node)
                : pageAddrBegin(_pageAddr), pageAddrEnd(_pageAddr + _pageCount), node(_node) {}

            inline bool contains(const Address pageAddr) const {
                return (pageAddr >= pageAddrBegin) && (pageAddr < pageAddrEnd);
            }

            inline size_t count() const {
                return pageAddrEnd - pageAddrBegin;
            }
        };

        class PageChunk {
        private:
            InList<PageRange> chunk;

        public:
            uint32_t lookup(Address pageAddr) const {
                // Look up in chunk to get the node for the page.
                auto pr = chunk.front();
                while (pr != nullptr) {
                    if (pr->contains(pageAddr)) return pr->node;
                    pr = pr->next;
                }
                return NUMAMap::INVALID_NODE;
            }

            size_t add(Address pageAddr, size_t pageCount, const uint32_t node) {
                // Add to chunk and keep the order, and merge if necessary.
                // Return number of pages that already exist and thus are ignored.
                size_t ignoredCount = 0;
                auto pr = chunk.front();
                while (pr != nullptr) {
                    if (pageAddr + pageCount < pr->pageAddrBegin) {
                        // No overlap with following ones, insert as new entry.
                        auto newpr = new PageRange(pageAddr, pageCount, node);
                        safeInsertAfter(pr->prev, newpr);
                        return ignoredCount;
                    } else if (pageAddr > pr->pageAddrEnd) {
                        // No overlap but after the current one, do nothing.
                        pr = pr->next;
                    } else if (node == pr->node) {
                        // Overlaps with current one and the same node.
                        // remove current one and merge into the new one.
                        auto mergeStart = std::min(pageAddr, pr->pageAddrBegin);
                        auto mergeEnd = std::max(pageAddr + pageCount, pr->pageAddrEnd);
                        pageAddr = mergeStart;
                        pageCount = mergeEnd - mergeStart;
                        auto q = pr;
                        pr = pr->next;
                        chunk.remove(q);
                        delete q;
                        if (pageCount == 0) {
                            return ignoredCount;
                        }
                    } else {
                        // Overlaps with current one but different nodes.
                        // Split into overlap and non-overlap ranges.
                        size_t numPagesBefore = pr->pageAddrBegin > pageAddr ?
                            pr->pageAddrBegin - pageAddr : 0;
                        size_t numPagesAfter = (pageAddr + pageCount) > pr->pageAddrEnd ?
                            (pageAddr + pageCount) - pr->pageAddrEnd : 0;

                        // Overlap range is ignored.
                        assert(numPagesBefore <= pageCount);
                        assert(numPagesAfter <= pageCount);
                        ignoredCount += pageCount - numPagesBefore - numPagesAfter;
                        // Non-overlap range before the current one is added directly.
                        if (numPagesBefore > 0) {
                            auto newpr = new PageRange(pageAddr, numPagesBefore, node);
                            safeInsertAfter(pr->prev, newpr);
                        }
                        // Non-overlap range after the current one becomes the new one.
                        if (numPagesAfter > 0) {
                            pageAddr = pr->pageAddrEnd;
                            pageCount = numPagesAfter;
                        } else {
                            return ignoredCount;
                        }
                        pr = pr->next;
                    }
                }
                // Push to the end.
                auto newpr = new PageRange(pageAddr, pageCount, node);
                chunk.push_back(newpr);
                return ignoredCount;
            }

            void remove(Address pageAddr, size_t pageCount) {
                // Remove from chunk and keep the order, and split if necessary.
                auto pr = chunk.front();
                while (pr != nullptr) {
                    if (pageAddr + pageCount < pr->pageAddrBegin) {
                        // No overlap with following ones, return.
                        return;
                    } else if (pageAddr > pr->pageAddrEnd) {
                        // No overlap but after the current one, continue.
                        pr = pr->next;
                    } else {
                        // Overlaps with current one.

                        // Decide range after split.
                        size_t numPagesBefore = pageAddr > pr->pageAddrBegin ?
                            pageAddr - pr->pageAddrBegin : 0;
                        size_t numPagesAfter = pr->pageAddrEnd > (pageAddr + pageCount) ?
                            pr->pageAddrEnd - (pageAddr + pageCount) : 0;
                        assert(numPagesBefore <= pr->count());
                        assert(numPagesAfter <= pr->count());

                        // Add split ranges.
                        // insertAfter(pr->prev) always adds right before pr.
                        if (numPagesBefore > 0) {
                            auto newpr = new PageRange(pr->pageAddrBegin, numPagesBefore, pr->node);
                            safeInsertAfter(pr->prev, newpr);
                        }
                        if (numPagesAfter > 0) {
                            auto newpr = new PageRange(pageAddr + pageCount, numPagesAfter, pr->node);
                            safeInsertAfter(pr->prev, newpr);
                        }

                        // Remove original range.
                        auto q = pr;
                        pr = pr->next;
                        chunk.remove(q);
                        delete q;
                    }
                }
            }

            void verify(uint64_t chunkIdx) const {
#if 0
                auto pageAddrBase = chunkIdx << chunkBits;
                uint64_t pageAddrMask = ~((1<<chunkBits) - 1);

                auto pr = chunk.front();
                uint64_t lastPageAddrEnd = 0;
                uint64_t lastNode = NUMAMap::INVALID_NODE;
                while (pr != nullptr) {
                    assert_msg((pr->pageAddrBegin & pageAddrMask) == pageAddrBase,
                            "Begin page addr 0x%lx does not match chunk base addr 0x%lx",
                            pr->pageAddrBegin, pageAddrBase);
                    assert_msg((pr->pageAddrEnd & pageAddrMask) == pageAddrBase,
                            "End page addr 0x%lu does not match chunk base addr 0x%lx",
                            pr->pageAddrEnd, pageAddrBase);
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
            void safeInsertAfter(PageRange* prev, PageRange* e) {
                if (prev == nullptr) chunk.push_front(e);
                else chunk.insertAfter(prev, e);
            }
        };

        static constexpr uint32_t chunkBits = 24;  // 2^24 pages, i.e., 64 GB

        g_unordered_map<uint64_t, PageChunk> pageMaps;

        lock_t futex;
};


constexpr uint32_t NUMAMap::INVALID_NODE;

NUMAMap::NUMAMap(const char* _patchRoot, const uint32_t numCores)
    : patchRoot(_patchRoot == nullptr ? "" : _patchRoot),
      coreNodeMap(numCores, INVALID_NODE), pageNodeMap(new PageMap())
{
    uint64_t pageSize = 4096;  // getpagesize()
    assert(isPow2(pageSize));
    pageBits = ilog2(pageSize);
    assert(pageBits > lineBits);

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
    return pageNodeMap->get(pageAddr);
}

size_t NUMAMap::addPagesToNode(const Address pageAddr, const size_t pageCount, const uint32_t node) {
    return pageNodeMap->add(pageAddr, pageCount, node);
}

void NUMAMap::removePages(const Address pageAddr, const size_t pageCount) {
    pageNodeMap->remove(pageAddr, pageCount);
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

