/** $lic$
 * Copyright (C) 2015 by Massachusetts Institute of Technology
 *
 * This file is part of libspin.
 *
 * libspin is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * libspin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "pin/pin.H"

#include <algorithm>
#include <array>
#include <vector>
#include <queue>
#include <cstdlib>
#include <random>
#include <string>

#include <iostream>
#include <set>
#include <map>
#include <sstream>
#include <unistd.h>

#ifndef SPIN_SLOW
#error "You must compile this file with SPIN_SLOW"
#endif

#include "mutex.h"
#include "assert.h"
#include "spin.h"
#include "log.h"

#define DEBUG(args...)
//#define DEBUG(args...) info(args)

namespace spin {

// Thread context state (2Kthreads is Pin's current limit)
#define MAX_THREADS 2048
std::array<CONTEXT, MAX_THREADS> contexts;

// FIXME: Shared with spin_fast.cpp!! Move to common file.
enum ThreadState  {
    UNCAPTURED, // Out in a syscall or other point out of our control. Will trip a capture point when it comes back to Pin; will trip before any other instrumentation function.
    BLOCKED,    // In program code, but blocked by the tool
    IDLE,       // Runnable but not active
    RUNNING,    // Currently running
    // Transitions: start -> UNCAPTURED
    //              On capture points: UNCAPTURED -> {IDLE, RUNNING}
    //              On switchpoints: IDLE <-> RUNNING
    //              On uncapture points: RUNNING -> UNCAPTURED
    //              On block/unblock: IDLE <-> BLOCKED
};

// Executor state (all strictly protected by executorMutex)
std::array<ThreadState, MAX_THREADS> threadStates;
std::array<mutex, MAX_THREADS> waitLocks;
volatile uint32_t executorTid;  // volatile b/c it's speculatively checked outside of a critical section
uint32_t curTid;
uint32_t capturedThreads;
bool executorInSyscall;
bool blockAfterSwitchcall;
aligned_mutex executorMutex;

REG executorReg;
REG switchReg;

// Callbacks, set on init
TraceCallback traceCallback = nullptr;
CaptureCallback captureCallback = nullptr;
UncaptureCallback uncaptureCallback = nullptr;
ThreadCallback threadStartCallback = nullptr;
ThreadCallback threadEndCallback = nullptr;

/* Tracing design in slow-mode SPIN (see the fast-mode comment first)
 *
 * Slow mode is ~100x slower than fast mode, but more robust and simpler.
 *
 * In slow mode, each normal analysis call works as-is without any extra
 * instrumentation. Each switchcall returns the next thread to run, and a
 * trailing SwitchHandler() uses SLOW PIN_ExecuteAt to switch to it.
 * 
 * Most of the smarts in slow-mode SPIN are in handling syscalls, which is
 * similar to fast-mode but without the context copies. As in fast mode,
 * a guard at the start of every trace handles captures, and syscalls are
 * prefaced with uncapture callbacks.
 */

/* Capture, uncapture, and executor handling */

void ThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v) {
    executorMutex.lock();
    DEBUG("Thread %d started", tid);
    threadStartCallback(tid);
    assert(threadStates[tid] == UNCAPTURED);
    PIN_SetContextReg(ctxt, executorReg, 0);  // will be captured immediately
    executorMutex.unlock();
}

void ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v) {
    executorMutex.lock();
    DEBUG("Thread %d finished", tid);
    if (threadStates[tid] == RUNNING) {
        assert(capturedThreads == 1);
        // This is the last thread, nothing to do. We do not call
        // uncaptureCallback, but the tool can detect termination by seeing the
        // thread count go to 0.
        // FIXME: Race between thread creation and exit?
    } else {
        assert(threadStates[tid] == UNCAPTURED);
    }
    threadEndCallback(tid);
    executorMutex.unlock();
}

/* Tracing sequence*/

// Helper method for guards. Must be called with executorMutex held
void UncaptureAndSwitch() {
    uint64_t nextTid = uncaptureCallback(curTid, (ThreadContext*)&contexts[curTid]);
    if (nextTid >= MAX_THREADS) panic("Switchcall returned invalid tid %d", nextTid);
    if (threadStates[nextTid] != IDLE) {
        panic("Switchcall returned tid %d, which is not IDLE (state[%d] = %d, curTid = %d executorTid = %d)",
                nextTid, nextTid, threadStates[nextTid], curTid, executorTid);
    }
    
    capturedThreads--;
    assert(threadStates[curTid] == RUNNING);
    threadStates[curTid] = UNCAPTURED;
    curTid = nextTid;
    assert(threadStates[curTid] == IDLE);
    threadStates[curTid] = RUNNING;
}

uint64_t RunTraceGuard(uint64_t executor) {
    return !executor;
}

// Runs only if we're coming back from a syscall
void TraceGuard(THREADID tid, const CONTEXT* ctxt) {
    assert(PIN_GetContextReg(ctxt, executorReg) == 0);
    DEBUG("[%d] In TraceGuard() (curTid %d rip 0x%lx er %d) [%d %d %d]", tid, curTid,
            PIN_GetContextReg(ctxt, REG_RIP), PIN_GetContextReg(ctxt, executorReg),
            threadStates[0], threadStates[1], threadStates[2]);
    executorMutex.lock();
    PIN_SaveContext(ctxt, &contexts[tid]);
    
    if (threadStates[tid] == RUNNING) {
        // We did not yield executor role when we ran the syscall, so keep
        // going as usual
        assert(executorTid == tid);
        assert(curTid == tid);
        assert(capturedThreads == 1);
        executorInSyscall = false;
        DEBUG("[%d] TG: Single thread, becoming executor", tid);
        executorMutex.unlock();
        PIN_SetContextReg(&contexts[tid], executorReg, 1);
        PIN_SetContextReg(&contexts[tid], switchReg, curTid);
        PIN_ExecuteAt(&contexts[tid]);
    }

    assert(threadStates[tid] == UNCAPTURED);
    bool runsNext = (capturedThreads == 0);
    captureCallback(tid, runsNext);
    capturedThreads++;
    threadStates[tid] = IDLE;

    if (runsNext) {
        DEBUG("[%d] TG: Only captured thread", tid);
        // We're the first! Make us run
        threadStates[tid] = RUNNING;
        assert(curTid == -1u);
        curTid = tid;
    }

    if (executorInSyscall) {
        DEBUG("[%d] TG: Executor is in syscall, running delayed uncapture", tid);
        assert(curTid == executorTid);
        assert(capturedThreads == 2);  // the non-uncaptured executor and us
        // Do delayed uncapture
        UncaptureAndSwitch();
        executorTid = -1u;
        executorInSyscall = false;
    }

    // If somebody else is the executor, wait until we're woken up, either
    // because we need to run a syscall or become the executor
    while (executorTid != -1u) {
        executorMutex.unlock();
        waitLocks[tid].lock();
        executorMutex.lock();
        if (threadStates[tid] == UNCAPTURED) {
            // Take syscall
            DEBUG("[%d] TG: Wakeup, taking own syscall", tid);
            executorMutex.unlock();
            PIN_SetContextReg(&contexts[tid], executorReg, 0);
            PIN_SetContextReg(&contexts[tid], switchReg, -1);
            PIN_ExecuteAt(&contexts[tid]);
        } else if (executorTid == -1u) {
            // NOTE: Due to wakeups interleaving with uncaptures, we can have
            // multiple threads going for the executor. For example, this
            // thread could have been woken up to claim executor, but a thread
            // that came out of a syscall got it first. Thus waking up does not
            // automatically mean executorTid == -1u, so we check.
            DEBUG("[%d] TG: Wakeup to claim executor", tid);
            break;
        } else {
            DEBUG("[%d] TG: Spurious wakeup", tid);
        }
    }

    assert(executorTid == -1u);

    // Become executor
    executorTid = tid;
    assert(curTid < MAX_THREADS);
    DEBUG("[%d] TG: Becoming executor, (curTid = %d, capturedThreads = %d)", tid, curTid, capturedThreads);
    executorMutex.unlock();
    PIN_SetContextReg(&contexts[curTid], executorReg, 1);
    PIN_SetContextReg(&contexts[curTid], switchReg, curTid);
    PIN_ExecuteAt(&contexts[curTid]);
}

uint64_t RunSyscallGuard(uint64_t executor) {
    return executor;
}

void SyscallGuard(THREADID tid, const CONTEXT* ctxt) {
    executorMutex.lock();
    DEBUG("[%d] In SyscallGuard() (curTid %d rip 0x%lx er %d)", tid, curTid,
            PIN_GetContextReg(ctxt, REG_RIP), PIN_GetContextReg(ctxt, executorReg));
 
    assert(executorTid == tid);
    PIN_SaveContext(ctxt, &contexts[curTid]);

    // Three possibilities:
    if (curTid != tid) {
        // 1. We need to ship off this syscall and move on to another thread 
        assert(capturedThreads >= 2);  // both us and the tid we're running must be captured
        uint32_t wakeTid = curTid;
        UncaptureAndSwitch();  // changes curTid
        waitLocks[wakeTid].unlock();  // wake syscall taker
        DEBUG("[%d] SG: Shipping syscall to real tid %d, running %d", tid, wakeTid, curTid);
        executorMutex.unlock();
        PIN_SetContextReg(&contexts[curTid], executorReg, 1);
        PIN_SetContextReg(&contexts[curTid], switchReg, curTid);
        PIN_ExecuteAt(&contexts[curTid]);
    } else {
        // We ourselves need to take the syscall...
        if (capturedThreads >= 2) {
            // 2. Wake up another idle thread to continue execution
            // Instead of searching for an idle non-executor thread, we
            // leverage that the thread we switch to must be captured, and make
            // that the executor as well.
            UncaptureAndSwitch();  // changes curTid
            executorTid = -1u;
            DEBUG("[%d] SG: Waking real tid %d, now running %d, and going to syscall", tid, curTid, curTid);
            waitLocks[curTid].unlock();  // wake new executor
        } else {
            // 3. We're the only captured thread, so if we uncaptured ourselves
            // the tool would run out of threads. Instead, let the first
            // captured thread do a delayed uncapture (or we'll do one)
            DEBUG("[%d] SG: Delayed uncapture", tid);
            assert(!executorInSyscall);
            executorInSyscall = true;
        }
        
        executorMutex.unlock();
        // Take our syscall
        PIN_SetContextReg(&contexts[tid], executorReg, 0);
        PIN_SetContextReg(&contexts[tid], switchReg, -1);
        PIN_ExecuteAt(&contexts[tid]);
    }
}

uint64_t NeedsSwitch(uint64_t nextTid) {
    return (nextTid != curTid) | blockAfterSwitchcall;
}

void SwitchHandler(THREADID tid, const CONTEXT* ctxt) {
    uint32_t nextTid = PIN_GetContextReg(ctxt, switchReg);
    executorMutex.lock();
    if (PIN_GetContextReg(ctxt, executorReg) != 1) {
        panic("[%d] I was supposed to be the executor?? But it's %d", tid, executorTid);
    }
    if (blockAfterSwitchcall && nextTid == curTid) {
        panic("[%d] Switchcall from thread %d called blockAfterSwitch(), but returned the same thread!", tid, curTid);
    }

    assert(executorTid == tid);
    assert(nextTid != curTid);  // o/w NeedsSwitch would prevent us from running
    assert(curTid <= MAX_THREADS);
    if (nextTid >= MAX_THREADS || threadStates[nextTid] != IDLE) {
        panic("[%d] Switchcall returned invalid next tid %d (state %d)", tid,
                nextTid, (nextTid < MAX_THREADS)? threadStates[nextTid] : -1);
    }

    PIN_SaveContext(ctxt, &contexts[curTid]);
    PIN_SetContextReg(&contexts[curTid], executorReg, 0);
    PIN_SetContextReg(&contexts[curTid], switchReg, -1);

    DEBUG("[%d] Switching %d -> %d", tid, curTid, nextTid);
    assert(threadStates[curTid] == RUNNING);
    if (!blockAfterSwitchcall) {
        threadStates[curTid] = IDLE;
    } else {
        threadStates[curTid] = BLOCKED;
        capturedThreads--;
        blockAfterSwitchcall = false;
    }

    curTid = nextTid;
    threadStates[curTid] = RUNNING;
    executorMutex.unlock();
    PIN_SetContextReg(&contexts[curTid], executorReg, 1);
    PIN_SetContextReg(&contexts[curTid], switchReg, curTid);
    PIN_ExecuteAt(&contexts[curTid]);
}


/* Tracing */

void Trace(TRACE trace, VOID *v) {
    // Order the trace's instructions
    std::vector<INS> idxToIns;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            idxToIns.push_back(ins);
        }
    }

    TraceInfo pt;
    pt.firstIns = idxToIns[0];
    pt.skipLeadingSwitchCall = INS_IsSyscall(pt.firstIns);
    traceCallback(trace, pt);

    if (!INS_IsSyscall(idxToIns[0])) {
        INS_InsertIfCall(idxToIns[0], IPOINT_BEFORE, (AFUNPTR)RunTraceGuard,
                IARG_REG_VALUE, executorReg,
                IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_END);
        INS_InsertThenCall(idxToIns[0], IPOINT_BEFORE, (AFUNPTR)TraceGuard,
                IARG_THREAD_ID, IARG_CONST_CONTEXT, 
                IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_END);
    }

    // Syscall guard
    for (INS ins : idxToIns) {
        if (INS_IsSyscall(ins)) {
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)RunSyscallGuard,
                    IARG_REG_VALUE, executorReg,
                    IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_END);
            INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)SyscallGuard,
                    IARG_THREAD_ID, IARG_CONST_CONTEXT, 
                    IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_END);
        }
    }

    // Switch handler
    // NOTE: For now, this is just a post-handler, but if we find we need to
    // modify the context in the switchcall (e.g., write arguments etc), we can
    // save the context first, pass our internal copy to the switchcall, then
    // run ExecuteAt.
    for (auto iip : pt.switchpoints) {
        INS ins = std::get<0>(iip);
        IPOINT ipoint = std::get<1>(iip);
        if (ipoint != IPOINT_BEFORE) {
            // We can probably do AFTER and TAKEN_BRANCH in slow mode, but
            // they're difficult to do in fast mode.
            panic("Switchcalls only support IPOINT_BEFORE for now");
        }

        if (ins == idxToIns[0] && INS_IsSyscall(ins)) continue;
        // Will be added right after the switchcall
        INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)NeedsSwitch,
                IARG_REG_VALUE, switchReg, IARG_END);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) SwitchHandler,
                IARG_THREAD_ID, IARG_CONST_CONTEXT, IARG_END);
    }
}

/* Public interface */

void init(TraceCallback traceCb, ThreadCallback startCb, ThreadCallback endCb, CaptureCallback captureCb, UncaptureCallback uncaptureCb) {
    for (auto& ts : threadStates) ts = UNCAPTURED;
    for (auto& wl : waitLocks) wl.lock();
    curTid = -1u;
    executorTid = -1u;
    executorInSyscall = false;
    blockAfterSwitchcall = false;
    capturedThreads = 0;

    traceCallback = traceCb;
    threadStartCallback = startCb;
    threadEndCallback = endCb;
    captureCallback = captureCb;
    uncaptureCallback = uncaptureCb;
    
    executorReg = PIN_ClaimToolRegister();
    switchReg = PIN_ClaimToolRegister();

    TRACE_AddInstrumentFunction(Trace, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
}

uint64_t getReg(const ThreadContext* tc, REG reg) {
    return PIN_GetContextReg((const CONTEXT*)tc, reg);
}

void setReg(ThreadContext* tc, REG reg, uint64_t val) {
    PIN_SetContextReg((CONTEXT*)tc, reg, val);
}

void saveContext(const ThreadContext* tc, CONTEXT* pinCtxt) {
    PIN_SaveContext((const CONTEXT*)tc, pinCtxt);
}

void loadContext(const CONTEXT* pinCtxt, ThreadContext* tc) {
    PIN_SaveContext(pinCtxt, (CONTEXT*)tc);
}

ThreadContext* getContext(ThreadId tid) {
    assert(tid < MAX_THREADS);
    assert(threadStates[tid] == BLOCKED || threadStates[tid] == IDLE);
    return (ThreadContext*)&contexts[tid];
}

REG __getSwitchReg() {
    assert(traceCallback);  // o/w not initialized
    return switchReg;
}

void blockAfterSwitch() {
    assert(!blockAfterSwitchcall);
    blockAfterSwitchcall = true;  // honored by SwitchHandler
}

void blockIdleThread(ThreadId tid) {
    executorMutex.lock();
    assert(tid < MAX_THREADS);
    assert(threadStates[tid] == IDLE);
    assert(capturedThreads > 1);
    threadStates[tid] = BLOCKED;
    capturedThreads--;
    executorMutex.unlock();
}

void unblock(ThreadId tid) {
    executorMutex.lock();
    assert(tid < MAX_THREADS);
    assert(threadStates[tid] == BLOCKED);
    threadStates[tid] = IDLE;
    capturedThreads++;
    executorMutex.unlock();
}

}  // namespace spin
