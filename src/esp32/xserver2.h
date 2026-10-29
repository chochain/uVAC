/// -*- mode: c++ -*-
#ifndef _XSERVER2_H
#define _XSERVER2_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include "xsvr_actor.h"

#define FORM_BUF_SZ 2048

class XServer {
private:
    uint16_t       _port;
    const char     *_ssid;
    const char     *_password;
    httpd_handle_t _httpd = nullptr;
    ServerActor    *_coor = nullptr;       ///< session coordinator
    
    void setup();

public:
    XServer(const char* ssid, const char* password, uint16_t port = 80) :
        _ssid(ssid), _password(password), _port(port) {}

    bool begin(ServerActor *actor, int priority);
    void conn(int sockfd, httpd_handle_t handle, char *buf);
    bool read_form(httpd_req_t *req, char *out, size_t out_sz, size_t &out_len);
    bool read_form_psram(httpd_req_t *req, char **out_psram_ptr);
};

#endif // _XSERVER2_H
