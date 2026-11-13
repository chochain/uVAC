/// -*- mode: c++ -*-
///
#ifndef _XSVR_ACTOR_H
#define _XSVR_ACTOR_H

#include "xactor.h"
#include "../forth_api.h"

#define REQ_TIMEOUT_MS 5000
#define MAX_SESSIONS   8

struct SessionState {
    uint32_t       sid        = 0;        ///< session id
    uint32_t       tid        = 0;        ///< task slot id
    uint32_t       key        = 0;        ///< browser-tab id (abort + VM ownership)
    int            fd         = -1;       ///< file handle
    httpd_handle_t hd         = nullptr;  ///< HTTP session handle
    bool           is_active  = false;    ///< flag: session active (slot occupied)
    char           *mem_block = nullptr;  ///< session input buffer pointer (PSRAM)
    size_t         read_idx   = 0;        ///< buffer current pos
    size_t         total_len  = 0;        ///< buffer total length
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
    int find_key(uint32_t key) {
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (_ses[i].is_active && _ses[i].key == key) return i;
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
    
    void finish_slot(int slot, bool abort=false) {
        if (slot < 0 || !_ses[slot].is_active) return;
        
        SessionState &s = _ses[slot];
        
        session_release(s.tid, abort);     /// * keeps the tab's data stack
        if (s.fd >= 0 && s.hd && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            httpd_socket_send(s.hd, s.fd, "0\r\n\r\n", 5, 0);
            xSemaphoreGive(_mutex);
        }

        XDEBUG("  %s.%u HANGUP sent. Session cleanly closed.\n", actor_name, s.sid);
        s.is_active = false;
        s.sid       = 0;
        s.tid       = 0;
        s.key       = 0;
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

    void register_conn(
        uint32_t key, uint32_t sid,
        int fd, httpd_handle_t hd, char* psram_buf, size_t len) {
        Guard g(_smtx);
        int slot = get_empty_slot();
        if (slot == -1) { /* unchanged: log, free, reject, return */ }

        int tid = session_attach(key, sid);   ///< the tab's persistent Forth VM
        if (tid <= 0) {
            XLOG("\n%s> %s for tab %u!\n", actor_name,
                tid < 0 ? "previous line still running" : "no available Forth task", key);
            heap_caps_free(psram_buf);
            reject(hd, fd);
            return;
        }
        SessionState &s = _ses[slot];
        s.sid       = sid;
        s.tid       = (uint32_t)tid;
        s.key       = key;
        s.fd        = fd;
        s.hd        = hd;
        s.is_active = true;
        s.mem_block = psram_buf; 
        s.total_len = len;
        s.read_idx  = 0;
        
        // Flush HTTP Chunked Transfer Encoding initial response headers line-by-line
        const char* headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n\r\n";
        httpd_socket_send(hd, fd, headers, strlen(headers), 0);

        XDEBUG("  %s.%u Session created on VM %u, HEADER sent.\n", actor_name, sid, tid);
        /// Begin your execution sequence loop [1]
        dispatch_next_line(slot);
    }
    ///
    ///> stream directory back to http (inter-core called directly from xforth_actor)
    ///
    void oob(uint32_t sid, const char *data, size_t len) override {
        Guard g(_smtx);
        int slot = find_slot(sid);
        if (slot < 0) return;           /// closed or aborted: drop
        send_chunk(_ses[slot].hd, _ses[slot].fd, data, len);
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
        int slot = (msg.type == MSG_FORTH_ABORT)
            ? find_key(msg.sid) : find_slot(msg.sid);
        if (slot < 0) return;

        SessionState &s = _ses[slot];

        switch (msg.type) {
        case MSG_FORTH_EOL:
            XDEBUG("  %s.%u << EOL => EOL_ACK", actor_name, msg.sid);
            dispatch_next_line(slot);
            break;
            
        case MSG_FORTH_DONE:
            XDEBUG("  %s.%u << DONE", actor_name, msg.sid);
            clean(s, "");
            finish_slot(slot, false);
            break;

        case MSG_FORTH_ABORT:
            XDEBUG("  %s.%u << ABORT", actor_name, msg.sid);
            clean(s, "\r\n[SYSTEM INTERRUPT] Execution Aborted via Panel.\r\n");
            finish_slot(slot, true);
            break;

        default:
            XLOG("  %s.%u << Unknown msg.type=%d\n", actor_name, msg.sid, msg.type);
            break;
        }
    }
};

#endif // _XSVR_ACTOR_H
