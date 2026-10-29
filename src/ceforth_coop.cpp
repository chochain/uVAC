///
/// @file ceforth_task.cpp
/// @brief eForth - 1970s Chuck Moore Cooperative Multi-tasking Engine for ESP32
///
#include "ceforth.h"

int  VM::curr_vm_id = 0;

#if !DO_MULTITASK
VM _vm0;                           ///< singleton, no VM pooling
VM& vm_get(int id) { return _vm0; }/// * return the singleton
void uvar_init() {
    U8 *b = &pmem[pmem.idx++];     ///< *base
    *b = 10;                       /// * default 10
    _vm0.id   = 0;                 /// * VM id
    _vm0.base = (IU)(b - MEM0);    /// * base idx
    pmem.idx  = ALIGN16(pmem.idx); /// 16-byte aligned
}

#else // DO_MULTITASK
///
///> VM messaging and IO control variables
///
int  VM::NCORE   = 1;              ///< default 1, updated in init
bool VM::io_busy = false;

List<VM, E4_VM_POOL_SZ> _vm;       ///< Pool of distinct VM contexts

VM& vm_get(int id) {
    return _vm[(id >= 0 && id < E4_VM_POOL_SZ) ? id : 0];
}

// =====================================================================
// 🏢 1970s Cooperative Scheduler Ring Execution
// =====================================================================

void t_pool_init() {
    // Overriding the hardware thread calculation; everything runs cooperatively on 1 core.
    VM::curr_vm_id = 0;       /// * Main task starts as active leader
    LOG("[SYSTEM] Cooperative Tasker initialized across [%d] slots\n", E4_VM_POOL_SZ);
}

void t_pool_stop() {
    // No physical threads to join or cleanup anymore
}

void uvar_init() {
    for (int i = 0; i < E4_VM_POOL_SZ; i++) {
        U8 *b = &pmem[pmem.idx++];                ///< *base
        *b = 10;                                  /// * default 10
        _vm[i].id   = i;                          /// * VM id
        _vm[i].base = (IU)(b - MEM0);             /// * base idx
        _vm[i].state = STOP;                      /// * Start out dead
    }
    _vm[0].state = QUERY;                         /// * Main loop task is alive
    pmem.idx = DALIGN(pmem.idx);                  /// DU aligned
}

int task_create(IU pfa) {
    // Find an empty task slot available backwards through the pool ring
    int i = E4_VM_POOL_SZ - 1;
    while (i > 0 && _vm[i].state != STOP) --i;
    
    if (i > 0) {
        _vm[i].reset(pfa, HOLD);                  /// Initialized and ready to be started
    }
    return i;
}

void task_start(int tid) {
    if (tid == 0) return;
    if (tid > 0 && tid < E4_VM_POOL_SZ) {
        _vm[tid].state = NEST;                    /// Mark it ready for scheduler traversal passes
    }
}

/// ===================================================================
/// 🔄 The Round-Robin Dispatch Loop (The Core 'PAUSE' Manager)
/// ===================================================================

/// This replaces your old thread-blocking `nest(vm)` architecture. 
/// Calling this shifts focus smoothly to the next task in line.
void cooperative_yield(void) {
    int next_tid = VM::curr_vm_id;
    
    // Walk down the circular array looking for the next execution-ready task context
    for (int i = 0; i < E4_VM_POOL_SZ; i++) {
        next_tid = (next_tid + 1) % E4_VM_POOL_SZ;
        if (_vm[next_tid].state == NEST || _vm[next_tid].state == QUERY) {
            VM::curr_vm_id = next_tid;
            return; 
        }
    }
    
    // Fallback: If everything else is sleeping/holding, return back to the main user shell task
    VM::curr_vm_id = 0;
    
    // Crucial ESP32 Guardrail: feed the watchdog when nothing else is demanding cycles
    delay(1); 
}

void VM::stop() {
    state = STOP;
    cooperative_yield();
}

void VM::join(int tid) {
    VM &target = vm_get(tid);
    // Keep giving away our execution slice until the target finishes completely
    while (target.state != STOP) {
        cooperative_yield();
    }
}

void VM::send(int tid, int n) {
    VM& dest = vm_get(tid);
    // Yield loops until destination goes to HOLD state (ready to absorb values)
    while (dest.state != HOLD) {
        cooperative_yield();
    }
    
    _ss_dup(dest, *this, n);
    dest.state = NEST; // Wake it up
    cooperative_yield();
}

void VM::recv() {
    state = HOLD; // Block ourselves waiting for an influx of variables
    cooperative_yield(); // Let other items run until someone rewrites our state back to NEST
}

void VM::bcast(int n) {
    // Intentional stub
}

void VM::pull(int tid, int n) {
    VM& src = vm_get(tid);
    while (src.state != STOP) {
        cooperative_yield();
    }
    _ss_dup(*this, src, n);
}

void VM::io_lock() {
    // Cooperatively wait for the IO lock to become completely vacant
    while (VM::io_busy) {
        cooperative_yield();
    }
    VM::io_busy = true;
}

void VM::io_unlock() {
    VM::io_busy = false;
    cooperative_yield();
}

void VM::reset(IU cfa, vm_state st) {
    rs.idx     = ss.idx = 0;
    ip         = cfa;
    tos        = -DU1;
    state      = st;
    compile    = false;
    *(MEM0 + base) = 10;
}

void VM::_ss_dup(VM &dst, VM &src, int n) {
    dst.ss.push(dst.tos);            
    dst.tos = src.tos;               
    src.tos = src.ss[-n];            
    for (int i = n - 1; i > 0; --i) {
        dst.ss.push(src.ss[-i]);     
    }
    src.ss.idx -= n;                 
}

#endif // DO_MULTITASK
