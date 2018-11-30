#include <numaif.h>
#include "cpuenum.h"
#include "log.h"
#include "memory_hierarchy.h"
#include "numa_map.h"
#include "virt/common.h"
#include "zsim.h"

#define BITS_PER_ULONG (sizeof(unsigned long) * 8)
#define ULONGS_FOR_BIT(n) (((n)+BITS_PER_ULONG-1)/BITS_PER_ULONG)

/* Help functions. */

// Manipulate nodemask. Return error code.
// nodemask -> vector<bool>.
static inline int nodemask2vector(unsigned long* nodemask, unsigned long maxnode, g_vector<bool>& vec) {
    // Initialize to empty.
    uint32_t sysMaxNode = zinfo->numaMap->getMaxNode();
    vec.assign(sysMaxNode + 1, false);

    // The number of nodes in nodemask.
    // It is a little confusing what maxnode means. If nodemask is not nullptr, maxnode is included;
    // but if nodemask is nullptr, 0 maxnode means no nodes are specified at all.
    if (nodemask == nullptr) return 0;
    // Only look at nodes up to max node.
    size_t num = MIN(sysMaxNode, maxnode) + 1;

    for (size_t iw = 0; iw < ULONGS_FOR_BIT(num); iw++) {
        unsigned long m;
        if (!safeCopy(nodemask + iw, &m)) return EFAULT;
        for (size_t ib = 0; ib < BITS_PER_ULONG; ib++) {
            size_t idx = ib + iw * BITS_PER_ULONG;
            if (idx >= num) break;
            vec[idx] = m & (1uL << ib);
        }
    }
    return 0;
}

// vector<bool> -> nodemask.
static inline int vector2nodemask(const g_vector<bool>& vec, unsigned long* nodemask, unsigned long maxnode) {
    // Check input vector.
    if (vec.empty()) return 0;
    uint32_t sysMaxNode = zinfo->numaMap->getMaxNode();
    assert(vec.size() == sysMaxNode + 1);

    // The number of nodes in nodemask.
    if (maxnode < sysMaxNode) return EINVAL;
    size_t num = sysMaxNode + 1;

    for (size_t iw = 0; iw < ULONGS_FOR_BIT(num); iw++) {
        unsigned long m = 0;
        for (size_t ib = 0; ib < BITS_PER_ULONG; ib++) {
            size_t idx = ib + iw * BITS_PER_ULONG;
            if (idx >= num) break;
            if (vec[idx]) m |= (1uL << ib);
        }
        if (!safeCopy(&m, nodemask + iw)) return EFAULT;
    }
    return 0;
}

// Empty vector<bool> (all false).
static inline bool isEmptyVector(const g_vector<bool>& vec) {
    for (const auto& b : vec) if (b) return false;
    return true;
}


// Validate policy and nodemask (as vector<bool>).
static inline int validate(int mode, const g_vector<bool>& vec, const char* name) {
#ifdef MPOL_F_STATIC_NODES
#ifdef MPOL_F_RELATIVE_NODES
    if ((mode & MPOL_F_STATIC_NODES) || (mode & MPOL_F_RELATIVE_NODES)) {
        warn("%s does not support MPOL_F_STATIC_NODES or MPOL_F_RELATIVE_NODES!", name);
        return EINVAL;
    }
#endif  // MPOL_F_RELATIVE_NODES
#endif  // MPOL_F_STATIC_NODES

    switch (mode) {
        case MPOL_DEFAULT:
#ifdef MPOL_LOCAL
        case MPOL_LOCAL:
#endif  // MPOL_LOCAL
            // Nodemask must be empty.
            if (!isEmptyVector(vec)) return EINVAL;
            break;
        case MPOL_BIND:
        case MPOL_INTERLEAVE:
            // Nodemask must be non-empty.
            if (isEmptyVector(vec)) return EINVAL;
            break;
        case MPOL_PREFERRED:
            // Nodemask could be empty or non-empty.
            break;
        default:
            // Invalid mode.
            return EINVAL;
    }
    return 0;
}


// Core-to-node mapping.
static inline uint32_t getNodeOfCore(uint32_t cid) {
    assert(cid < zinfo->numCores);
    return zinfo->numaMap->getNodeOfCore(cid);
}


// Address-to-node mapping.
static inline Address getPageAddress(void* addr) {
    return zinfo->numaMap->getPageAddress((Address)addr);
}

static inline Address getPageAddressEnd(void* addr, unsigned long len) {
    return getPageAddress(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) + len - 1)) + 1;
}

static inline uint32_t getNodeOfAddr(void* addr) {
    return zinfo->numaMap->getNodeOfPage(getPageAddress(addr));
}

static inline size_t addAddrRangeToNode(void* addr, unsigned long len, uint32_t node) {
    auto begin = getPageAddress(addr);
    auto end = getPageAddressEnd(addr, len);
    return zinfo->numaMap->addPagesToNode(begin, end - begin, node);
}

static inline void removeAddrRange(void* addr, unsigned long len) {
    auto begin = getPageAddress(addr);
    auto end = getPageAddressEnd(addr, len);
    zinfo->numaMap->removePages(begin, end - begin);
}

static inline size_t addAddrRangeThreadPolicy(void* addr, unsigned long len, uint32_t tid, uint32_t cid, NUMAPolicy* policy = nullptr) {
    auto begin = getPageAddress(addr);
    auto end = getPageAddressEnd(addr, len);
    return zinfo->numaMap->addPagesThreadPolicy(begin, end - begin, tid, cid, policy);
}


/* Patches. */

PostPatchFn getErrorPostPatch(int err) {
    return [err](PostPatchArgs args) {
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
        return PPA_NOTHING;
    };
}

// SYS_get_mempolicy
PostPatchFn PatchGetMempolicy(PrePatchArgs args) {
    if (!zinfo->numaMap) {
        warn("[%d] NUMA is not modeled in the simulated system configuration, syscall: SYS_get_mempolicy (%d)", args.tid, SYS_get_mempolicy);
        return getErrorPostPatch(ENOSYS);
    }

    int* mode = reinterpret_cast<int*>(PIN_GetSyscallArgument(args.ctxt, args.std, 0));
    unsigned long* nodemask = reinterpret_cast<unsigned long*>(PIN_GetSyscallArgument(args.ctxt, args.std, 1));
    unsigned long maxnode = PIN_GetSyscallArgument(args.ctxt, args.std, 2);
    void* addr = reinterpret_cast<void*>(PIN_GetSyscallArgument(args.ctxt, args.std, 3));
    unsigned long flags = PIN_GetSyscallArgument(args.ctxt, args.std, 4);

    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)SYS_getpid);  // no effect on host

    // Validate.
    if (((flags & MPOL_F_ADDR) && addr == nullptr) || (!(flags & MPOL_F_ADDR) && addr != nullptr)
            || ((flags & MPOL_F_MEMS_ALLOWED) && ((flags & MPOL_F_ADDR) || (flags & MPOL_F_NODE))))
        return getErrorPostPatch(EINVAL);

    return [=](PostPatchArgs args){
        if (!flags) {
            // Return policy through mode and nodemask.
            const auto& policy = zinfo->numaMap->getThreadPolicy(args.tid);
            if (mode != nullptr) {
                int resMode = static_cast<int>(policy.getMode());
                if (!safeCopy(&resMode, mode)) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EFAULT);
                    return PPA_NOTHING;
                }
            }
            if (nodemask != nullptr) {
                const auto& resMask = policy.getMask();
                auto err = vector2nodemask(resMask, nodemask, maxnode);
                if (err) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
                    return PPA_NOTHING;
                }
            }
        } else if (flags & MPOL_F_MEMS_ALLOWED) {
            // Return allowed nodes through nodemask. Argument mode is ignored.
            if (nodemask != nullptr) {
                // By default all nodes are allowed for mbind().
                g_vector<bool> resMask(zinfo->numaMap->getMaxNode() + 1, true);
                auto err = vector2nodemask(resMask, nodemask, maxnode);
                if (err) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
                    return PPA_NOTHING;
                }
            }
        } else if ((flags & MPOL_F_ADDR) && (flags & MPOL_F_NODE)) {
            // Return node ID in mode for addr.
            const auto node = getNodeOfAddr(addr);
            if (mode != nullptr) {
                int resMode = static_cast<int>(node);
                if (!safeCopy(&resMode, mode)) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EFAULT);
                    return PPA_NOTHING;
                }
            }
            if (nodemask != nullptr) {
                g_vector<bool> resMask(zinfo->numaMap->getMaxNode() + 1, false);
                resMask[node] = true;
                auto err = vector2nodemask(resMask, nodemask, maxnode);
                if (err) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
                    return PPA_NOTHING;
                }
            }
        } else if (flags & MPOL_F_ADDR) {
            // Return policy for addr through mode and nodemask, if not null.
            // FIXME(mgao12): currently we do not store the allocation policy.
            warn("SYS_get_mempolicy does not support MPOL_F_ADDR for allocation policy!");
            PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EINVAL);
            return PPA_NOTHING;
        } else if (flags & MPOL_F_NODE) {
            // Return next interleaving node ID.
            const auto& policy = zinfo->numaMap->getThreadPolicy(args.tid);
            if (policy.getMode() != MPOL_INTERLEAVE) {
                // The policy must be MPOL_INTERLEAVE.
                PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EINVAL);
                return PPA_NOTHING;
            }
            const auto nextNode = zinfo->numaMap->getThreadNextAllocNode(args.tid);
            assert(nextNode != NUMAMap::INVALID_NODE);
            if (mode != nullptr) {
                int resMode = static_cast<int>(nextNode);
                if (!safeCopy(&resMode, mode)) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EFAULT);
                    return PPA_NOTHING;
                }
            }
        } else {
            // Invalid flags.
            PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EINVAL);
            return PPA_NOTHING;
        }

        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)0);  // return 0 on success
        return PPA_NOTHING;
    };
}

// SYS_set_mempolicy
PostPatchFn PatchSetMempolicy(PrePatchArgs args) {
    if (!zinfo->numaMap) {
        warn("[%d] NUMA is not modeled in the simulated system configuration, syscall: SYS_set_mempolicy (%d)", args.tid, SYS_set_mempolicy);
        return getErrorPostPatch(ENOSYS);
    }

    int mode = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    unsigned long* nodemask = reinterpret_cast<unsigned long*>(PIN_GetSyscallArgument(args.ctxt, args.std, 1));
    unsigned long maxnode = PIN_GetSyscallArgument(args.ctxt, args.std, 2);

    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)SYS_getpid);  // no effect on host

    // Translate nodemask.
    g_vector<bool> vec;
    int err = nodemask2vector(nodemask, maxnode, vec);
    if (err) return getErrorPostPatch(err);

    // Validate.
    err = validate(mode, vec, "SYS_set_mempolicy");
    if (err) return getErrorPostPatch(err);

    // Update policy.
    return [mode, vec](PostPatchArgs args) {
        zinfo->numaMap->setThreadPolicy(args.tid, mode, vec);
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)0);  // return 0 on success
        return PPA_NOTHING;
    };
}

// SYS_mbind
PostPatchFn PatchMbind(PrePatchArgs args) {
    if (!zinfo->numaMap) {
        warn("[%d] NUMA is not modeled in the simulated system configuration, syscall: SYS_mbind (%d)", args.tid, SYS_mbind);
        return getErrorPostPatch(ENOSYS);
    }

    void* addr = reinterpret_cast<void*>(PIN_GetSyscallArgument(args.ctxt, args.std, 0));
    unsigned long len = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    int mode = PIN_GetSyscallArgument(args.ctxt, args.std, 2);
    unsigned long* nodemask = reinterpret_cast<unsigned long*>(PIN_GetSyscallArgument(args.ctxt, args.std, 3));
    unsigned long maxnode = PIN_GetSyscallArgument(args.ctxt, args.std, 4);
    unsigned long flags = PIN_GetSyscallArgument(args.ctxt, args.std, 5);

    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)SYS_getpid);  // no effect on host

    // Translate nodemask.
    g_vector<bool> vec;
    int err = nodemask2vector(nodemask, maxnode, vec);
    if (err) return getErrorPostPatch(err);

    // Validate.
    err = validate(mode, vec, "SYS_mbind");
    if (err) return getErrorPostPatch(err);
    if (flags & MPOL_MF_MOVE_ALL) {
        warn("SYS_mbind does not support MPOL_MF_MOVE_ALL!");
        return getErrorPostPatch(EPERM);
    }

    // We must get the core info now, since thread will leave after entering syscall.
    uint32_t cid = getCid(args.tid);
    assert_msg(cid < zinfo->numCores, "Thread %u runs on core %u? Are we in FF?", args.tid, cid);

    return [=](PostPatchArgs args) {
        // Construct the policy if not default.
        NUMAPolicy* policy = nullptr;
        if (mode != MPOL_DEFAULT) {
            policy = new NUMAPolicy(mode, vec);
        }

        // Add all non-existing pages; either move or ignore existing pages depending on flags.
        bool isStrict = (flags & MPOL_MF_STRICT);
        bool movePages = (flags & MPOL_MF_MOVE);
        if (movePages) {
            removeAddrRange(addr, len);
        }
        auto ignoredCount = addAddrRangeThreadPolicy(addr, len, args.tid, cid, policy);
        if (isStrict && ignoredCount != 0) {
            // Some pages do not follow the policy or could not be moved.
            PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EIO);
            return PPA_NOTHING;
        }

        delete policy;

        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)0);  // return 0 on success
        return PPA_NOTHING;
    };
}

// SYS_migrate_pages
PostPatchFn PatchMigratePages(PrePatchArgs args) {
    if (!zinfo->numaMap) {
        warn("[%d] NUMA is not modeled in the simulated system configuration, syscall: SYS_migrate_pages (%d)", args.tid, SYS_migrate_pages);
        return getErrorPostPatch(ENOSYS);
    }

    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)SYS_getpid);  // no effect on host

    return [](PostPatchArgs args) {
        // FIXME(mgao12): current NUMAMap does not provide interface to migrate all pages
        // associated with a node in a process, so we do not patch migrate_page for now.
        warn("SYS_migrate_pages is not supported for now!");
        // Make it a failure.
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EPERM);
        return PPA_NOTHING;
    };
}

// SYS_move_pages
PostPatchFn PatchMovePages(PrePatchArgs args) {
    if (!zinfo->numaMap) {
        warn("[%d] NUMA is not modeled in the simulated system configuration, syscall: SYS_move_pages (%d)", args.tid, SYS_move_pages);
        return getErrorPostPatch(ENOSYS);
    }

    uint32_t linuxTid = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    unsigned long count = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    void** pages = reinterpret_cast<void**>(PIN_GetSyscallArgument(args.ctxt, args.std, 2));
    const int* nodes = reinterpret_cast<const int*>(PIN_GetSyscallArgument(args.ctxt, args.std, 3));
    int* status = reinterpret_cast<int*>(PIN_GetSyscallArgument(args.ctxt, args.std, 4));
    int flags = PIN_GetSyscallArgument(args.ctxt, args.std, 5);

    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)SYS_getpid);  // no effect on host

    // Validate.
    if (linuxTid != 0 || (flags & MPOL_MF_MOVE_ALL)) {
        warn("SYS_move_pages does not support non-zero pid or MPOL_MF_MOVE_ALL!");
        return getErrorPostPatch(EPERM);
    }
    if (pages == nullptr) return getErrorPostPatch(EINVAL);

    return [=](PostPatchArgs args) {
        int err = 0;
        for (unsigned long idx = 0; idx < count; idx++) {
            // Get page.
            void* page = nullptr;
            if (!safeCopy(pages + idx, &page)) {
                err = EFAULT;
                break;
            };

            int stat = 0;
            if (nodes != nullptr) {
                // Move pages.
                int resNode = 0;
                if (!safeCopy(nodes + idx, &resNode)) {
                    err = EFAULT;
                    break;
                }
                uint32_t node = static_cast<uint32_t>(resNode);
                if (node > zinfo->numaMap->getMaxNode()) {
                    err = ENODEV;
                    break;
                }
                removeAddrRange(page, 1);
                assert(addAddrRangeToNode(page, 1, node) == 0);
                stat = static_cast<int>(node);
            } else {
                // Get current node.
                uint32_t node = getNodeOfAddr(page);
                stat = static_cast<int>(node);
            }
            if (status != nullptr) {
                if (!safeCopy(&stat, status + idx)) {
                    err = EFAULT;
                    break;
                }
            }
        }

        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
        return PPA_NOTHING;
    };
}

// SYS_munmap
PostPatchFn PatchMunmap(PrePatchArgs args) {
    if (zinfo->numaMap) {
        void* addr = reinterpret_cast<void*>(PIN_GetSyscallArgument(args.ctxt, args.std, 0));
        size_t len = PIN_GetSyscallArgument(args.ctxt, args.std, 1);

        removeAddrRange(addr, len);
    }

    return NullPostPatch;
}

// SYS_mremap
PostPatchFn PatchMremap(PrePatchArgs args) {
    if (zinfo->numaMap) {
        void* old_addr = reinterpret_cast<void*>(PIN_GetSyscallArgument(args.ctxt, args.std, 0));
        size_t old_size = PIN_GetSyscallArgument(args.ctxt, args.std, 1);

        warn("mremap will NOT preserve the NUMA memory policy with the original allocation!");

        removeAddrRange(old_addr, old_size);
    }

    return NullPostPatch;
}

