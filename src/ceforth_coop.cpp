///
/// @file ceforth_coop.cpp
/// @brief eForth - cooperative multitasking: PAUSE + TASK + START, web sessions as tasks
///
#include "ceforth.h"

#if DO_MULTITASK && !USE_PTHREAD
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern List<VM, E4_VM_POOL_SZ> _vm;           ///< VM pool defined in ceforth_task.cpp
extern IU gStop[];                            ///< defined in ceforth.cpp

int g_compiler = -1;                          ///< tid with an open ':' definition (-1 = none, 0 = shell)
int g_cur      = 0;                           ///< tid of the slice now running

VM& vm_get(int id) {
    return _vm[(id >= 0 && id < E4_VM_POOL_SZ) ? id : 0];
}

///
///> engine lock: one slice of Forth at a time (dictionary, pmem, tib and obuf are shared)
///
static SemaphoreHandle_t _lock = nullptr;

bool forth_lock(int ms) {
    if (!_lock) return true;                  /// * before t_pool_init: single threaded
    return xSemaphoreTakeRecursive(_lock, ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(ms)) == pdTRUE;
}
void forth_unlock() { if (_lock) xSemaphoreGiveRecursive(_lock); }
int  forth_cur()    { return g_cur; }

void t_pool_init() {
    if (!_lock) _lock = xSemaphoreCreateRecursiveMutex();
    LOG("[SYSTEM] cooperative tasker, %d slots\n", E4_VM_POOL_SZ);
}
void t_pool_stop() {}

void uvar_init() {
    for (int i = 0; i < E4_VM_POOL_SZ; i++) {
        U8 *b = &pmem[pmem.idx++];
        *b = 10;                              /// * default radix
        _vm[i].id    = i;
        _vm[i].base  = (IU)(b - MEM0);
        _vm[i].state = STOP;                  /// * free slot
    }
    _vm[0].state = QUERY;                     /// * the shell
    pmem.idx = ALIGN4(pmem.idx);              /// * was DALIGN, a no-op
}

///
///> slot allocation: lock-free, the httpd thread, dispatcher workers and Forth words all call in
///
static int claim_slot(vm_state to) {          ///< returns slot (1..N-1) or 0 if full
    for (int i = E4_VM_POOL_SZ - 1; i > 0; i--) {
        int expect = STOP;
        if (__atomic_compare_exchange_n((int*)&_vm[i].state, &expect, (int)to,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return i;
    }
    return 0;
}

int task_create(IU pfa) {                     ///< returns tid, or 0 if the pool is full
    int i = claim_slot(HOLD);                 /// * created, not yet running
    if (i) _vm[i].reset(pfa, HOLD);
    else   ERR("task slots full\n");
    return i;
}

void task_start(int tid) {
    if (tid > 0 && tid < E4_VM_POOL_SZ && _vm[tid].state == HOLD && !_vm[tid].sess) {
        _vm[tid].state = NEST;                /// * now in the ring
    }
}

///
///> web session = task
///  A session slot is a shell VM (state QUERY) that is never in the ring: it is only
///  ever driven by forth_run(), one slice per ForthActor message.
///
int session_open(uint32_t sid) {              ///< returns tid, or 0 if the pool is full
    int i = claim_slot(HOLD);
    if (!i) return 0;

    VM &v   = _vm[i];
    v.ss.idx = v.rs.idx = 0;
    v.ip    = 0;
    v.compile = false;
    v.wake  = 0;
    v.fb    = 0;
    v.tib   = nullptr;
    v.tbuf  = nullptr;
    v.sid   = sid;
    v.sess  = true;
    *(MEM0 + v.base) = 10;                    /// * default radix
    v.state = QUERY;                          /// * owned, idle
    return i;
}

bool session_close(int tid) {
    if (tid <= 0 || tid >= E4_VM_POOL_SZ) return false;
    VM &v = _vm[tid];
    if (!v.sess) return false;
    if (!forth_lock(250)) {                   /// * a slice of this VM may be running
        LOG("[SYSTEM] session_close(%d) timed out\n", tid);
        return false;
    }
    if (g_compiler == tid) g_compiler = -1;   /// * release an abandoned ':' definition
    free(v.tbuf);
    v.tbuf = nullptr;
    v.tib  = nullptr;
    v.sid  = 0;                               /// * stale RESUME msgs now fail the sid check
    v.sess = false;
    v.compile = false;
    v.wake = 0;
    v.ss.idx = v.rs.idx = 0;
    v.state = STOP;                           /// * last: slot is free again
    forth_unlock();
    return true;
}

/// One turn of the ring: run each runnable task whose wake time has passed.
/// Returns true if a task ran, or if only sleepers remain (after a 1ms nap),
/// so the drain loop in outer_poll() keeps going until they finish.
static bool one_turn() {
    auto due = [](U32 wake) { return (S32)(millis() - wake) >= 0; };   /// * rollover-safe
    bool ran = false, sleep = false;
    for (int i = 1; i < E4_VM_POOL_SZ; i++) {
        VM &t = _vm[i];
        if (t.state != NEST || t.sess) continue;  /// * sessions are driven by forth_run()
        if (!due(t.wake)) { sleep = true; continue; }
        nest(t);                              /// * returns at its pause/ms, or at doSTOP
        ran = true;
    }
    if (!ran && sleep) { delay(1); return true; }
    return ran;
}

/// ms <= 0: one turn of the ring (pause, PAUSE(), handle_idle).
/// ms >  0: shell wait, keep the ring turning for ms milliseconds.
bool task_pause(int ms) {
    if (ms <= 0) return one_turn();

    bool busy = false;
    U32  t0   = millis();
    do {
        if (one_turn()) busy = true;
        else            delay(1);         /// * no tasks at all, don't spin
    } while ((S32)(millis() - t0) < ms);
    return busy;
}
#endif // DO_MULTITASK && !USE_PTHREAD
