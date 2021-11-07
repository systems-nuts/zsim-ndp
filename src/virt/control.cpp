#include "log.h"
#include "process_tree.h"
#include "scheduler.h"
#include "virt/common.h"
#include "zsim.h"

static void sleepUntilPhase(uint32_t tid, uint64_t wakeupPhase, CONTEXT* ctxt, SYSCALL_STANDARD std) {
    auto futexWord = zinfo->sched->markForSleep(procIdx, tid, wakeupPhase);
    // Turn this into a non-timed FUTEX_WAIT syscall
    PIN_SetSyscallNumber(ctxt, std, SYS_futex);
    PIN_SetSyscallArgument(ctxt, std, 0, (ADDRINT)futexWord);
    PIN_SetSyscallArgument(ctxt, std, 1, (ADDRINT)FUTEX_WAIT);
    PIN_SetSyscallArgument(ctxt, std, 2, (ADDRINT)1 /*by convention, see sched code*/);
    PIN_SetSyscallArgument(ctxt, std, 3, (ADDRINT)nullptr);
}

PostPatchFn PatchExitGroup(PrePatchArgs args) {
    if (args.isNopThread || zinfo->procArray[procIdx]->isInFastForward()) {
        // Already in FF, i.e., left the barrier. No need to patch.
        return NullPostPatch;
    }
    /* We need to play a trick here. If we directly do exit_group, other
     * threads may be killed without calling leave() first, resulting in
     * deadlock at the phase barrier. Our solution is to mark the process as in
     * a group-exit state, which every thread will check at the next end of
     * phase, and call leave() at the beginning of next phase. The caller waits
     * until then and re-executes exit_group, to finish the whole process.
     */
    // Mark the process as in group-exit.
    info("PatchExitGroup: thread %u in process %u calls exit_group", args.tid, procIdx);
    zinfo->procArray[procIdx]->exitGroup();

    // Save args.
    ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    ADDRINT arg1 = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    ADDRINT arg2 = PIN_GetSyscallArgument(args.ctxt, args.std, 2);
    ADDRINT arg3 = PIN_GetSyscallArgument(args.ctxt, args.std, 3);
    // Save current PC for retry.
    ADDRINT prevIp = PIN_GetContextReg(args.ctxt, REG_INST_PTR);

    // Sleep for 2 phases until other threads leave in the next phase.
    uint64_t wakeupPhase = zinfo->numPhases + 2;
    sleepUntilPhase(args.tid, wakeupPhase, args.ctxt, args.std);

    return [wakeupPhase, prevIp, arg0, arg1, arg2, arg3](PostPatchArgs args) {
        if (wakeupPhase > zinfo->numPhases) {
            warn("PatchExitGroup: thread was waken up too early (current %lu < expected %lu); retry", zinfo->numPhases, wakeupPhase);
            sleepUntilPhase(args.tid, wakeupPhase, args.ctxt, args.std);
        } else {
            // Re-execute exit_group
            PIN_SetSyscallNumber(args.ctxt, args.std, SYS_exit_group);
            // Restore pre-call args
            PIN_SetSyscallArgument(args.ctxt, args.std, 0, arg0);
            PIN_SetSyscallArgument(args.ctxt, args.std, 1, arg1);
            PIN_SetSyscallArgument(args.ctxt, args.std, 2, arg2);
            PIN_SetSyscallArgument(args.ctxt, args.std, 3, arg3);
        }
        PIN_SetContextReg(args.ctxt, REG_INST_PTR, prevIp);
        return PPA_USE_RETRY_PTRS;
        // A successful exit_group will never return, which ends retry.
    };
}

