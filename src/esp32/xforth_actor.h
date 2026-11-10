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
        if (!sid) return;                           /// * closed or aborted: drop output

        BaseActor *svr = Sys.get_actor(SERVER_ACTOR_ID);
        BaseActor *gui = Sys.get_actor(GUI_ACTOR_ID);

        for (int off = 0; off < len; off += QUE_BUF_SZ - 1) {
            int sz = std::min(len - off, QUE_BUF_SZ - 1);

            ActorMsg fb { MSG_FORTH_FEEDBACK, SERVER_ACTOR_ID, sid, tid };
            memcpy(fb.buf, rst + off, sz);
            fb.buf[sz] = '\0';

            int tries = FB_RETRY;
            while (!Sys.send(fb, 0)) {              /// queue full: let the other workers drain it
                if (!forth_cur_sid() || --tries <= 0) {
                    LOG("  %s.%u output dropped (queue full)\n", svr->actor_name, sid);
                    return;                         /// not counted, so EOL handshake still balances
                }
                vTaskDelay(1);
            }
            forth_fb_add();                         /// per session, per line

            if (!gui) continue;
            ActorMsg g { MSG_GUI_TEXT, GUI_ACTOR_ID, sid, tid };
            memcpy(g.buf, rst + off, sz);
            g.buf[sz] = '\0';
            gui->receive(g);
        }
    }

public:
    ForthActor(uint32_t id, const char *name) : BaseActor(id, name) {
        LOG("VM> Forth Actor %u [%s] active on core[%d]\n", id, name, xPortGetCoreID());
    }

    void handle_run(const ActorMsg &msg) {
        int rc  = forth_run(
            msg.tid, msg.sid, msg.type==MSG_FORTH_EXEC ? msg.buf : nullptr, feedback);

        switch (rc) {
        case RUN_DONE: {                        /// line finished
            ActorMsg eof { MSG_FORTH_EOL, SERVER_ACTOR_ID, msg.sid, msg.tid };
            eof.buf[0]        = '\0';
            eof.feedback_sent = forth_fb(msg.tid);
            if (!post(eof)) LOG("  %s.%u EOL lost\n", actor_name, msg.sid);
            break;
        }
        case RUN_MORE:                          /// paused: runnable right away
        case RUN_WAIT: {                        /// sleeping: don't spin the queue
            if (rc == RUN_WAIT) vTaskDelay(1);
            ActorMsg again = msg;
            again.type = MSG_FORTH_RESUME;
            if (!post(again)) LOG("  %s.%u RESUME lost\n", actor_name, msg.sid);
            break;
        }
        case RUN_BUSY:                          /// engine or compiler taken: same request again
            vTaskDelay(1);
            if (!post(msg)) LOG("  %s.%u retry lost\n", actor_name, msg.sid);
            break;
        default: break;                         /// RUN_GONE: session was closed, nothing to report
        }
    }

    void receive(const ActorMsg &msg) override {
        switch (msg.type) {
        case MSG_FORTH_EXEC:
            LOG("  %s.%u << %s", actor_name, msg.sid, "EXEC");
            handle_run(msg);
            break;
            
        case MSG_FORTH_RESUME: {
            LOG("  %s.%u << %s", actor_name, msg.sid, "RESUME");
            handle_run(msg);
            break;
        }
        case MSG_FORTH_EOL_ACK:
            LOG("  %s.%u << EOL_ACK", actor_name, msg.sid);
            // The Coordinator confirmed network buffers are clear.
            // xsvr_actor already send MSG_FORTH_DONE to itself
            break;

        case MSG_GUI_TOUCH:
            DEBUG("  %s.%u << touch(%d, %d)", actor_name, msg.sid, msg.touch.x, msg.touch.y);
            break;

        default:
            LOG("  %s.%u << msg.type=%d not supported\n", actor_name, msg.sid, msg.type);
            break;
        }
    }
};
#endif // _XFORTH_ACTOR_H
