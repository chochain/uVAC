/// -*- mode: c++ -*-
#ifndef _XSERVER2_H
#define _XSERVER2_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include "xsvr_actor.h"

#define FORM_BUF_SZ 2048
#define MAX_TABS    16

struct Tab {
    char tok[33]; char tab[17]; uint32_t key = 0; uint32_t last = 0;
};

class XServer {
private:
    const char     *_ssid;                 ///< WiFi SSID
    const char     *_password;             ///< WiFi password
    uint16_t       _port;                  ///< WiFi port
    
    httpd_handle_t _httpd = nullptr;
    ServerActor    *_coor = nullptr;       ///< session coordinator
    
    uint32_t       _tick  = 0;             ///< return packet count
    Tab            _tabs[MAX_TABS];        ///< keep tab key
    
    void setup();

public:
    XServer(const char* ssid, const char* password, uint16_t port = 80) :
        _ssid(ssid), _password(password), _port(port) {}

    bool     begin(ServerActor *actor, int priority);
    void     conn(uint32_t key, int sockfd, httpd_handle_t handle, char *buf);
    uint32_t tab_key(const char *tok, const char *tab, bool create);
    bool     read_form(httpd_req_t *req, char **out_psram_ptr);
};

#endif // _XSERVER2_H
