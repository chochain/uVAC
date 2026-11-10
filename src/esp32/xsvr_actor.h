/// -*- mode: c++ -*-
#ifndef _XSVR_ACTOR_H
#define _XSVR_ACTOR_H

#include "xactor.h"
#include "../forth_api.h"

#define REQ_TIMEOUT_MS 5000
#define MAX_SESSIONS   8

struct SessionState {
    uint32_t       sid        = 0;        ///< session id
    uint32_t       tid        = 0;        ///< task slot id
    int            fd         = -1;       ///< file handle
    httpd_handle_t hd         = nullptr;  ///< HTTP session handle
    bool           is_active  = false;    ///< flag: session active (slot occupied)
    bool           eof_wait   = false;    ///< flag: waiting for EOF
    
    char           *mem_block = nullptr;  ///< session input buffer pointer (PSRAM)
    size_t         read_idx   = 0;        ///< buffer current pos
    size_t         total_len  = 0;        ///< buffer total length

    uint32_t       fb_count   = 0;        ///< feedback counter
    uint32_t       fb_total   = 0;        ///< feedback tally
};

class ServerActor : public BaseActor {
private:
    SessionState      _ses[MAX_SESSIONS];
    SemaphoreHandle_t _mutex;             ///< esp_http_server socket send mutex

    int find_slot(uint32_t sid) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (_ses[i].is_active && _ses[i].sid == sid) return i;
        }
        return -1;
    }

    int get_empty_slot() {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!_ses[i].is_active) {
                return i;                /// Found an available structural slot!
            }
        }
        return -1;                       /// session bucket full
    }
    
    void send_chunk(httpd_handle_t hd, int fd, const char *data, size_t len) {
        if (!data || len == 0 || fd < 0 || !hd) return;

        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            char hbuf[16];
            snprintf(hbuf, sizeof(hbuf), "%zx\r\n", len);
            httpd_socket_send(hd, fd, hbuf, strlen(hbuf), 0);
            httpd_socket_send(hd, fd, data, len, 0);
            httpd_socket_send(hd, fd, "\r\n", 2, 0);
            xSemaphoreGive(_mutex);
        }
    }

    void dispatch_next_line(int slot) {
        SessionState &s = _ses[slot];
    
        /// Wrap the entire parsing block inside a flat execution loop
        while (true) {
            if (s.read_idx >= s.total_len) {
                /// All text segments consumed. Safely trigger completion event.
                ActorMsg done { MSG_FORTH_DONE, SERVER_ACTOR_ID, s.sid, s.tid };
                done.buf[0] = '\0';
                Sys.send(done);
                return;               /// safe exit, 0 recursion risk
            }
            char line_buf[QUE_BUF_SZ];
            size_t w = 0;

            /// Extract characters until reaching a clean newline delimiter
            while (s.read_idx < s.total_len) {
                char c = s.mem_block[s.read_idx++];
            
                if (c == '\r' || c == '\0') continue;
                if (c == '\n') {
                    if (w > 0) break; /// complete command captured!
                    continue;
                }
                if (w < QUE_BUF_SZ - 1) {
                    line_buf[w++] = c;
                }
            }
            line_buf[w] = '\0';

            if (w > 0) {              /// valid command, send to Forth VM
                ActorMsg m { MSG_FORTH_EXEC, FORTH_ACTOR_ID, s.sid, s.tid };
                memcpy(m.buf, line_buf, w + 1);
                Sys.send(m);
                return;               /// line sent, wait for EOL handshake.
            }
            /// If w == 0, the loop naturally continues to evaluate the next line 
            /// without allocating any extra stack frames!
        }
    }
    
    void check_and_handshake(int slot) {
        SessionState &s = _ses[slot];
        
        /// Only proceed if we have received the EOL message
        /// and all feedback packets have arrived
        if (!s.eof_wait || (s.fb_count != s.fb_total)) return;
        
        /// Reset counters for the next line sequence step
        s.fb_count = 0;
        s.fb_total = 0;
        s.eof_wait = false;

        /// send ACK back to Forth Actor to release its lock state if any
        ActorMsg ack{ MSG_FORTH_EOL_ACK, FORTH_ACTOR_ID, s.sid, s.tid };
        ack.buf[0] = '\0';
        Sys.send(ack);

        /// 2. Move to the next string segment inside PSRAM
        dispatch_next_line(slot);
    }
    
    void finish_slot(int slot) {
        if (slot < 0 || !_ses[slot].is_active) return;
        
        SessionState &s = _ses[slot];
        
        session_close(s.tid);              /// * frees the Forth task slot
        if (s.fd >= 0 && s.hd && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            httpd_socket_send(s.hd, s.fd, "0\r\n\r\n", 5, 0);
            xSemaphoreGive(_mutex);
        }

        DEBUG("  %s.%u HANGUP sent. Session cleanly closed.\n", actor_name, s.sid);
        s.is_active = false;
        s.sid       = 0;
        s.tid       = 0;
        s.fd        = -1;
        s.hd        = nullptr;
    }

    void reject(httpd_handle_t hd, int fd) {
        static const char r[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        httpd_socket_send(hd, fd, r, sizeof(r) - 1, 0);
    }

    SemaphoreHandle_t _smtx;            // protects _ses[] (httpd thread + dispatcher workers)
    struct Guard {
        SemaphoreHandle_t m;
        Guard(SemaphoreHandle_t m) : m(m) { xSemaphoreTakeRecursive(m, portMAX_DELAY); }
        ~Guard() { xSemaphoreGiveRecursive(m); }
    };

public:
    ServerActor(uint32_t id, const char *name) : BaseActor(id, name) {
        _smtx  = xSemaphoreCreateRecursiveMutex();
        _mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(_mutex);
    }

    ~ServerActor() override {
        if (_mutex) vSemaphoreDelete(_mutex);
    }

    void register_new_conn(uint32_t sid, int fd, httpd_handle_t hd, char* psram_buf, size_t len) {
        Guard g(_smtx);
        int slot = get_empty_slot();
        if (slot == -1) {
            LOG("\n%s> Max session capacity %d hit! Rejecting request.\n", actor_name, MAX_SESSIONS);
            heap_caps_free(psram_buf); // Release allocation payload block
            reject(hd, fd);
            return;
        }

        int tid = session_open(sid);   ///< the session IS a Forth task (VM slot)
        if (tid <= 0) {
            LOG("\n%s> No available Forth task for session %u!\n", actor_name, sid);
            heap_caps_free(psram_buf);
            reject(hd, fd);
            return;
        }
        
        SessionState &s = _ses[slot];
        s.sid       = sid;
        s.tid       = (uint32_t)tid;
        s.fd        = fd;
        s.hd        = hd;
        s.is_active = true;
        s.eof_wait  = false;
        s.mem_block = psram_buf; 
        s.total_len = len;
        s.read_idx  = 0;
        s.fb_count  = 0;
        s.fb_total  = 0;
        
        // Flush HTTP Chunked Transfer Encoding initial response headers line-by-line
        const char* headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n\r\n";
        httpd_socket_send(hd, fd, headers, strlen(headers), 0);

        DEBUG("  %s.%u Session created on VM %u, HEADER sent.\n", actor_name, sid, tid);
        /// Begin your execution sequence loop [1]
        dispatch_next_line(slot);
    }

    void receive(const ActorMsg &msg) override {
        Guard g(_smtx);                 /// workers may deliver msgs of different sessions in parallel
        auto clean = [this](SessionState &s, const char *err) {
            if (s.mem_block != nullptr) {
                heap_caps_free(s.mem_block);
                s.mem_block = nullptr;
            }
            if (err) send_chunk(s.hd, s.fd, err, strlen(err));
        };
        int slot = find_slot(msg.sid);
        if (slot < 0) return;

        SessionState &s = _ses[slot];

        switch (msg.type) {
        case MSG_FORTH_FEEDBACK:
            DEBUG("  %s.%u << FB => '%s'\n", actor_name, msg.sid, (char*)msg.buf);
            s.fb_count++;               /// Track arrival
            send_chunk(s.hd, s.fd, msg.buf, strlen(msg.buf));
            
            // Check if this feedback was the last piece we were waiting for
            check_and_handshake(slot);
            break;

        case MSG_FORTH_EOL:
            DEBUG("  %s.%u << EOL => EOL_ACK\n", actor_name, msg.sid);
            // Forth VM completed its processing loop step and is waiting!
            // 1. Force the LwIP network buffers to clear out onto the physical wire
            // (Optional: flush hardware sockets if necessary)

            // Forth VM has finished generating, record the target number it sent
            s.fb_total = msg.feedback_sent;
            s.eof_wait = true;

            // Verify if all feedbacks are already here, or if we need to wait for late packets
            check_and_handshake(slot);
            break;
            
        case MSG_FORTH_DONE:
            DEBUG("  %s.%u << DONE\n", actor_name, msg.sid);
            clean(s, "");
            finish_slot(slot);
            break;

        case MSG_FORTH_ABORT:
            DEBUG("  %s.%u << ABORT\n", actor_name, msg.sid);
            clean(s, "\r\n[SYSTEM INTERRUPT] Execution Aborted via Panel.\r\n");
            finish_slot(slot);
            break;

        default:
            DEBUG("  %s.%u << Unknown msg.type=%d\n", actor_name, msg.sid, msg.type);
            break;
        }
    }
};

#endif // _XSVR_ACTOR_H
