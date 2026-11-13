/// -*- mode: c++ -*-
#ifndef _XFORTH_ACTOR_H
#define _XFORTH_ACTOR_H
#pragma once

#include "xactor.h"
#include "../forth_api.h"
///
/// Each web session owns a Forth task (VM slot, see session_open()).
/// This actor only schedules slices: EXEC starts a line, RESUME continues one
/// that paused or slept. forth_run() takes the engine lock, so any worker may
/// run it. Session state (sid, feedback count, input cursor) lives in the VM,
/// not in this class, which is why there are no statics here anymore.
///
class ForthActor : public BaseActor {
private:
    static constexpr int FB_RETRY = 200;            ///< ~200 ms of back-pressure, then drop

    static bool post(const ActorMsg &m, int tries = 100) {
        while (!Sys.send(m, 0)) {                   /// never block here: the queue's consumers are us
            if (--tries <= 0) return false;
            vTaskDelay(1);
        }
        return true;
    }

    /// Output hook, called from inside forth_run() (engine lock held).
    static void feedback(int len, const char *rst) {
        const uint32_t sid = forth_cur_sid();       ///< running session slice
        const uint32_t tid = forth_cur_tid();
        if (!sid) return;

        BaseActor *svr = Sys.get_actor(SERVER_ACTOR_ID);
        BaseActor *gui = Sys.get_actor(GUI_ACTOR_ID);

        if (svr) svr->oob(sid, rst, len);           /// in order, no queue, no size limit
        if (!gui) return;
        for (int off = 0; off < len; off += QUE_BUF_SZ - 8) {
            int sz = std::min(len - off, QUE_BUF_SZ - 8);
            ActorMsg g { MSG_GUI_TEXT, GUI_ACTOR_ID, sid, tid };
            snprintf(g.buf, QUE_BUF_SZ, "%d> %.*s", (int)tid, sz, rst + off);
            gui->receive(g);                        /// * send to mailbox (cross-core)
        }
    }
    
public:
    ForthActor(uint32_t id, const char *name) : BaseActor(id, name) {
        XLOG("VM> Forth Actor %u [%s] active on core[%d]\n", id, name, xPortGetCoreID());
    }

    void handle_run(const ActorMsg &msg) {
        int rc  = forth_run(
            msg.tid, msg.sid, msg.type==MSG_FORTH_EXEC ? msg.buf : nullptr, feedback);

        switch (rc) {
        case RUN_DONE: {                        /// line finished
            ActorMsg eof { MSG_FORTH_EOL, SERVER_ACTOR_ID, msg.sid, msg.tid };
            eof.buf[0] = '\0';
            if (!post(eof)) XLOG("  %s.%u EOL lost\n", actor_name, msg.sid);
            break;
        }
        case RUN_MORE:                          /// paused: runnable right away
        case RUN_WAIT: {                        /// sleeping: don't spin the queue
            vTaskDelay(1);                      /// * give a little
            ActorMsg again = msg;
            again.type = MSG_FORTH_RESUME;
            if (!post(again)) XLOG("  %s.%u RESUME lost\n", actor_name, msg.sid);
            break;
        }
        case RUN_BUSY:                          /// engine or compiler taken: same request again
            vTaskDelay(1);
            if (!post(msg)) XLOG("  %s.%u retry lost\n", actor_name, msg.sid);
            break;
        default: break;                         /// RUN_GONE: session was closed, nothing to report
        }
    }

    void receive(const ActorMsg &msg) override {
        switch (msg.type) {
        case MSG_FORTH_EXEC:
            XDEBUG("  %s.%u << %s", actor_name, msg.sid, "EXEC");
            handle_run(msg);
            break;
            
        case MSG_FORTH_RESUME: {
            XDEBUG("  %s.%u << %s", actor_name, msg.sid, "RESUME");
            handle_run(msg);
            break;
        }
        case MSG_GUI_TOUCH:
            XDEBUG("  %s.%u << touch(%d, %d)", actor_name, msg.sid, msg.touch.x, msg.touch.y);
            break;

        default:
            XLOG("  %s.%u << msg.type=%d not supported\n", actor_name, msg.sid, msg.type);
            break;
        }
    }
};
#endif // _XFORTH_ACTOR_H
