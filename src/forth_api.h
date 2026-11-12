/// -*- mode: c++ -*-
/// @file
/// @brief Narrow interface between the actor layer and the Forth engine.
///        Plain types only: no VM, no List, no config.h.
///
#ifndef _FORTH_API_H
#define _FORTH_API_H
#include <stdint.h>
#include "config.h"

#if DO_MULTITASK
#if USE_PTHREAD
#define FORTH_LOCK()
#define FORTH_UNLOCK()

#else // !USE_PTHREAD
///@name Web sessions as tasks (cooperative tasker)
///@{
bool    forth_lock(int ms);                 ///< engine lock (recursive), ms<0 = wait forever
void    forth_unlock(bool vm0=false);
#define FORTH_LOCK()     forth_lock(-1)
#define FORTH_UNLOCK(f)  forth_unlock(f)
///@}

enum {                                      ///< forth_run() results
    RUN_WAIT =  2,                          ///< sleeping (ms), call again later
    RUN_DONE =  1,                          ///< line consumed
    RUN_MORE =  0,                          ///< paused, runnable now: call again with line=NULL
    RUN_BUSY = -1,                          ///< engine/compiler taken: call again, SAME arguments
    RUN_GONE = -2                           ///< session closed or aborted: drop it
};

int      session_open(uint32_t sid);        ///< claim a task slot for a web session, 0 if pool full
bool     session_close(uint32_t tid);       ///< release it (waits briefly for the engine lock)
int      session_attach(uint32_t key, uint32_t sid);       ///< >0 tid, 0 pool full, -1 tab busy
bool     session_release(uint32_t tid, bool abort=false);  ///< end a request, KEEP the stack

/// One slice of a session's line. line!=NULL starts a line, NULL resumes it.
/// hook(len, text) receives output while the slice runs (engine lock held).
int      forth_run(uint32_t tid, uint32_t sid,
                   const char *line, void (*hook)(int, const char*) = nullptr);

/// Valid only inside the output hook:
uint32_t forth_cur_sid();                   ///< session id of the running slice (0 = closed/none)
uint32_t forth_cur_tid();                   ///< its task id

#endif // USE_PTHREAD
#endif // DO_MULTITASK
#endif // _FORTH_API_H
