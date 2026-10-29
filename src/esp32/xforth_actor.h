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
    static uint32_t          _active_tid;           ///< active VM id
    static std::atomic<bool> _abort;                ///< abort flag
    static uint32_t          _feedback_cnt;
    
    static void feedback(int len, const char *rst) {
        if (_abort.load()) return;
    
        BaseActor *gui = Sys.get_actor(GUI_ACTOR_ID);

        for (int off = 0; off < len; off += QUE_BUF_SZ - 1) {
            int sz = std::min(len - off, QUE_BUF_SZ - 1);

            ActorMsg fb { MSG_FORTH_FEEDBACK, SERVER_ACTOR_ID, _active_sid, _active_tid };
            memcpy(fb.buf, rst + off, sz);
            fb.buf[sz] = '\0';
        
            // COOPERATIVE BACKPRESSURE CHECK:
            // Try to push to the queue. If it's full, do NOT block the core.
            // Instead, spill registers, yield control to other actors, and try again!
            while (!Sys.send(fb, 0)) { 
                // Save local C engine registers back into the active VM state memory
                // This allows the ServerActor to drain the queue on this exact worker thread!
                vm_get(_active_tid).state = HOLD;
                // Cooperatively pass the execution baton 
                vTaskDelay(pdMS_TO_TICKS(1)); // Feeds the FreeRTOS watchdog
            }
        
            ++_feedback_cnt;
            DEBUG("  [XF].%u >> FB <%d>%s", _active_sid, sz, fb.buf);
        
            if (!gui) continue; 
            ActorMsg g { MSG_GUI_TEXT, GUI_ACTOR_ID, _active_sid, _active_tid };
            memcpy(g.buf, rst + off, sz);
            g.buf[sz] = '\0';
            gui->receive(g); 
        }
    }

public:
    ForthActor(uint32_t actor_id) : BaseActor(actor_id) {
        _abort.store(false);
        LOG("VM> Cooperative Forth Actor active on core[%d]\n", xPortGetCoreID());
    }

    void receive(const ActorMsg &msg) override {
        switch (msg.type) {
        case MSG_FORTH_EXEC:
            DEBUG("  [XF].%d << EXEC '%s'\n", msg.sid, (char*)msg.buf);
            _active_sid = msg.sid;
            _active_tid = msg.tid;
            _abort.store(false);

            if (forth_vm(msg.tid, msg.buf, feedback)) {
                /// Fully processed! Clean finish.
                ActorMsg eof { MSG_FORTH_EOL, SERVER_ACTOR_ID, msg.sid, msg.tid };
                eof.buf[0] = '\0';
                eof.feedback_sent = _feedback_cnt;
                while (!Sys.send(eof, pdMS_TO_TICKS(50)));  
            } 
            else {
                /// Paused! Simply re-enqueue the same message to the back
                /// forth_vm picks up right where 'tib' left off.
                Sys.send(msg, pdMS_TO_TICKS(5));
            }
            break;

        case MSG_FORTH_EOL_ACK:
            DEBUG("  [XF].%u << EOL_ACK\n", msg.sid);
            // The Coordinator confirmed network buffers are clear.
            // xsvr_actor already send MSG_FORTH_DONE to itself
            break;

        case MSG_FORTH_ABORT:
            DEBUG("  [XF].%u << ABORT\n", msg.sid);
            if (msg.sid == _active_sid) _abort.store(true);
            break;

        case MSG_GUI_TOUCH:
            DEBUG("  [XF].%u << touch(%d, %d)\n", msg.sid, msg.touch.x, msg.touch.y);
            break;
            
        default:
            LOG("  [XF].%u << msg.type=%d not supported\n", msg.sid, msg.type);
            break;
        }
    }
};
#endif // _XFORTH_ACTOR_H
