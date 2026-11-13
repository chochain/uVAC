/// -*- mode: c++ -*-
/// @file
/// @brief Central Actor System definitions & thread-pool multiplexer
///
#ifndef _XACTOR_H
#define _XACTOR_H

#include <Arduino.h>
#include <map>
#include <algorithm>
#include "esp_http_server.h"

#define QUE_DEPTH       32
#define QUE_BUF_SZ      128
#define QUE_WAIT_TICKS  5

#define XLOG(fmt, ...)   Serial.printf(fmt, __VA_ARGS__)
//#define XDEBUG(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define XDEBUG(fmt, ...)

// Static Global Target Registry IDs
#define SERVER_ACTOR_ID 1
#define FORTH_ACTOR_ID  2
#define GUI_ACTOR_ID    3

enum ActorMsgType {
    MSG_FORTH_EXEC,           /// Forward individual raw command line to VM
    MSG_FORTH_RESUME,         /// Continue a session slice that paused or slept (no payload)
    MSG_FORTH_EOL,            /// Generation checkpoint completed by Forth
    MSG_FORTH_DONE,           /// Explicit macro execution processing complete
    MSG_FORTH_ABORT,          /// Emergency priority drop request
    MSG_SESSION_TIMEOUT,      /// Stagnation safety event
    MSG_GUI_TEXT,             /// Forward string outputs straight to the LVGL terminal
    MSG_GUI_DRAW,             /// Forward string outputs straight to the LVGL terminal
    MSG_GUI_TOUCH,            /// Touch coordinate packets dispatched from Core 1 to Core 0
    MSG_SYS_TELEMETRY,        /// Periodic hardware memory metric tracking frame
};

struct ActorMsg {
    ActorMsgType   type;
    uint32_t       target_id; /// Destination Actor target register
    uint32_t       sid;       /// Session ID tracking tag
    uint32_t       tid;       /// Forth Task/VM Pool id
    int            fd;        /// Socket file descriptor tracking hook
    httpd_handle_t hd;        /// Webserver process descriptor link
    
    union {
        char buf[QUE_BUF_SZ]; /// Inline 128-byte data transmission window
        struct {
            uint32_t xx1;
            int16_t  x;
            int16_t  y;
            uint8_t  state; 
        } touch;
        struct {
            uint32_t xx2;
            uint32_t free_heap_kb;
            uint32_t free_psram_kb;
        } memory;             /// 8-byte payload structure for live metrics
    };
};

class BaseActor {
public:
    const char *actor_name = nullptr;
    uint32_t   actor_id;
    BaseActor(uint32_t id, const char *name) : actor_id(id), actor_name(name) {}
    virtual ~BaseActor() {}
    virtual void oob(uint32_t sid, const char *data, size_t len) {}
    virtual void receive(const ActorMsg &msg) = 0;
};

class ActorSystem {
private:
    QueueHandle_t                  _queue;          /// map M workers => N actors
    std::map<uint32_t, BaseActor*> _registry;       /// id => actor
    SemaphoreHandle_t              _mutex;
    TaskHandle_t                  *_workers;
    int                            _worker_count;
    uint32_t                       _next_id;

    static void dispatch(void *pv) {
        ActorSystem *sys = static_cast<ActorSystem*>(pv);
        ActorMsg msg;
        while (1) {
            /// n-workers blocked here at 0% CPU until new msg arrived
            if (xQueueReceive(sys->_queue, &msg, portMAX_DELAY) == pdTRUE) {
                BaseActor *actor = sys->get_actor(msg.target_id);
                XDEBUG("\n%s> {%u:%u '%s'} >> [%s]",
                      pcTaskGetName(NULL), msg.sid, msg.type, (char*)msg.buf, actor->actor_name);
                if (actor) actor->receive(msg);
                /// no delay here, so no context switching
            }
        }
    }

public:
    ActorSystem() : _queue(nullptr), _workers(nullptr), _worker_count(0), _next_id(100) {
        _mutex = xSemaphoreCreateMutex();
    }

    void begin(int n, int priority) {
        _worker_count = n;
        _queue   = xQueueCreate(QUE_DEPTH, sizeof(ActorMsg));
        _workers = new TaskHandle_t[n];
        
        for (int i = 0; i < n; ++i) {
            char name[10];
            snprintf(name, sizeof(name), "W%d", i);
            xTaskCreatePinnedToCore(
                dispatch, name, 8192,
                this, priority, &_workers[i], 0);   /// Pinned to Core 0
        }
    }

    uint32_t alloc_id() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        uint32_t tid = ++_next_id;
        xSemaphoreGive(_mutex);
        return tid;
    }
    
    void register_actor(BaseActor *actor) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _registry[actor->actor_id] = actor;
        xSemaphoreGive(_mutex);
    }

    void unregister_actor(uint32_t id) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _registry.erase(id);
        xSemaphoreGive(_mutex);
    }

    /// Bounded-wait send. ONLY call from threads that are not this queue's
    /// consumer (Forth task, httpd thread). Never from a dispatcher worker.
    bool send(const ActorMsg &msg, TickType_t ticks=QUE_WAIT_TICKS, bool priority=false) {
        auto it = _registry.find(msg.target_id);
        BaseActor* actor = (it != _registry.end()) ? it->second : nullptr;
        if (!actor) return false;

        XDEBUG("\n%s> [%s] << {%u:%u '%s'}",
            pcTaskGetName(NULL), actor->actor_name, msg.sid, msg.type, (char*)msg.buf);

        if (!_queue) return false;
              
        bool rst = priority
             ? xQueueSendToFront(_queue, &msg, ticks) == pdPASS
             : xQueueSend(_queue, &msg, ticks) == pdPASS;
        
        XDEBUG("%s", rst ? "_" : " => queue full");
        
        return rst;
    }

    BaseActor *get_actor(uint32_t id) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        auto it = _registry.find(id);
        BaseActor* actor = (it != _registry.end()) ? it->second : nullptr;
        xSemaphoreGive(_mutex);
        return actor;
    }
};

extern ActorSystem Sys;
#endif // _XACTOR_H
