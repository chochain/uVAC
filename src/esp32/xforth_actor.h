/// -*- mode: c++ -*-
#ifndef _XFORTH_ACTOR_H
#define _XFORTH_ACTOR_H
#pragma once

#include "xactor.h"
#include <atomic>

extern int forth_vm(const char *cmd, void(*hook)(int, const char*));

class ForthActor : public BaseActor {
private:
    static uint32_t          _active_sid;           ///< active session
    static std::atomic<bool> _abort;                ///< abort flag
    static uint32_t          _feedback_cnt;

    static void feedback(int len, const char *rst) {
        if (_abort.load()) return;
        
        BaseActor *gui = Sys.get_actor(GUI_ACTOR_ID);

        for (int off = 0; off < len; off += QUE_BUF_SZ - 1) {
            int sz = std::min(len - off, QUE_BUF_SZ - 1);

            ActorMsg fb { MSG_FORTH_FEEDBACK, SERVER_ACTOR_ID, _active_sid };
            memcpy(fb.buf, rst + off, sz);
            fb.buf[sz] = '\0';
            if (Sys.send(fb, pdMS_TO_TICKS(200))) ++_feedback_cnt;   // count only what really got queued

            DEBUG("  [XF].%u >> FB <%d>%s", _active_sid, sz, fb.buf);
            
            if (!gui) continue;                                      // best effort, bypasses the shared queue
            ActorMsg g { MSG_GUI_DRAW_CMD, GUI_ACTOR_ID, _active_sid };
            memcpy(g.buf, rst + off, sz);
            g.buf[sz] = '\0';
            gui->receive(g);
        }
    }

public:
    ForthActor(uint32_t actor_id) : BaseActor(actor_id) {
        _abort.store(false);
        LOG("VM> Forth VM created on core[%d]\n", xPortGetCoreID());
    }

    void receive(const ActorMsg &msg) override {
        switch (msg.type) {
        case MSG_FORTH_EXEC:
            DEBUG("  [XF].%d << '%s'\n", msg.sid, (char*)msg.buf);
            _active_sid = msg.sid;
            _abort.store(false);
            _feedback_cnt = 0;

            forth_vm(msg.buf, feedback);
            
            // Command line processed. Send EOL and pause execution of next line.
            {
                ActorMsg eof { MSG_FORTH_EOL, SERVER_ACTOR_ID, msg.sid };
                eof.feedback_sent = _feedback_cnt;
                while (!Sys.send(eof, pdMS_TO_TICKS(50)));  /// keep trying
            }
            break;

        case MSG_FORTH_EOL_ACK:
            DEBUG("  [XF].%u << EOL_ACK\n", msg.sid);
            // The Coordinator confirmed network buffers are clear.
            // xsvr_actor already send MSG_FORTH_DONE to itself
            // Safe to close transaction, out-of-order free!
            // release lock if any
            break;

        case MSG_FORTH_ABORT:
            DEBUG("  [XF].%u << ABORT\n", msg.sid);
            if (msg.sid == _active_sid) {
                _abort.store(true);
            }
            break;

        case MSG_GUI_TOUCH_TRIGGER:
            LOG("  [XF].%u << touch(%d, %d)\n", msg.sid, msg.touch.x, msg.touch.y);
            break;
            
        default:
            LOG("  [XF].%u << msg.type=%d not supported\n", msg.sid, msg.type);
            break;
        }
    }
};
#endif // _XFORTH_ACTOR_H
