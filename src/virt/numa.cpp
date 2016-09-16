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

static inline uint32_t getNodeOfAddr(void* addr) {
    return zinfo->numaMap->getNodeOfPage(getPageAddress(addr));
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
            // See PatchSetMempolicy(), only allow MPOL_DEFAULT for now.
            // MPOL_DEFAULT associates with empty nodemask.
            if (mode != nullptr) {
                int resMode = MPOL_DEFAULT;
                if (!safeCopy(&resMode, mode)) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EFAULT);
                    return PPA_NOTHING;
                }
            }
            if (nodemask != nullptr) {
                g_vector<bool> resMask(zinfo->numaMap->getMaxNode() + 1, false);
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
            if (node == NUMAMap::INVALID_NODE) {
                // Currently no way to figure out the node for implicit bound address.
                // Make it a failure.
                PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EPERM);
                return PPA_NOTHING;
            }
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
            // See PatchSetMempolicy(), default policy is always MPOL_DEFAULT.
            int resMode = MPOL_DEFAULT;
            g_vector<bool> resMask(zinfo->numaMap->getMaxNode() + 1, false);
            const auto node = getNodeOfAddr(addr);
            if (node != NUMAMap::INVALID_NODE) {
                // Was done through mbind()
                resMode = MPOL_BIND;
                resMask[node] = true;
            }
            if (mode != nullptr) {
                if (!safeCopy(&resMode, mode)) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EFAULT);
                    return PPA_NOTHING;
                }
            }
            if (nodemask != nullptr) {
                auto err = vector2nodemask(resMask, nodemask, maxnode);
                if (err) {
                    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-err);
                    return PPA_NOTHING;
                }
            }
        } else if (flags & MPOL_F_NODE) {
            // Return next interleaving node ID.
            // See PatchSetMempolicy(), MPOL_INTERLEAVE is not supported, return EINVAL.
            PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EINVAL);
            return PPA_NOTHING;
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

    // FIXME(mgao12): only allow mempolicy of MPOL_DEFAULT.
    // In theory we could support MPOL_BIND and MPOL_PREFERRED, or even
    // MPOL_INTERLEAVE by having a per-processing array of size MAX_THREADS tracking
    // the policy of each thread, but this complicates the node info of the implicit
    // bound pages. Then it is needed to limit the change of bound node (e.g., one
    // node for each thread forever, etc.). Since this is dirty and not necessary, we
    // ignore them here. But if you fix this, make sure you also update
    // PatchGetMempolicy() to match.
    if (mode != MPOL_DEFAULT) {
        warn("SYS_set_mempolicy tries to set policy other than MPOL_DEFAULT, ignored!");
        return getErrorPostPatch(EINVAL);
    }

    return [](PostPatchArgs args) {
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)0);  // return 0 on success
        return PPA_NOTHING;
    };
}

