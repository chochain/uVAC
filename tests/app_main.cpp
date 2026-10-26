///
/// ESP-IDF: interactive Forth core as a FreeRTOS task over the console
///
/// @file
/// @brief uVAC - UNIVAC 1108 retro on ESP32
/// @note
///    benchmark1  : xx 999 for 34 drop next ; : yy 999 for xx next ;
///    1440ms Dr. Ting's orig/esp32forth_82
///    1240ms ~/Download/forth/esp32/esp32forth8_exp9
///    1045ms orig/esp32forth8_1
///     999ms orig/40x/ceforth subroutine-threaded, 16-bit xt offset
///     940ms orig/40x/ceforth use cached xt offsets in nest()
///     665ms src/ceforth vector-based, object-threaded
///     534ms src/ceforth, multi-threading, vector-based, object-threaded (with gcc -O3)
///     248ms ref: ESP32forth 7.0.8 (Arduino IDE, 240MHz)
///     383ms uvac: 32-bit single-threaded, lambda-trampoline (ESP-IDF, 160MHz)
///     232ms uvac: 32-bit single-threaded, lambda-trampoline (Arduino IDE, 240MHz)
///
///    benchmark2  : xx 999 for 34 i + drop next ; : yy 999 for xx next ;
///     
///     404ms ref: ESP32forth 7.0.8 (Arduino IDE, 240MHz)
///     589ms uvac: 32-bit single-threaded, lambda-trampoline (ESP-IDF, 160MHz)
///     383ms uvac: 32-bit single-threaded, lambda-trampoline (Arduino IDE, 240MHz)
///
#include <cstdio>
#include <ctime>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

using namespace std;

extern void forth_init();
extern int  forth_vm(const char *cmd, void(*)(int, const char*)=nullptr);
extern void forth_teardown();

const char* APP_VERSION = "uVAC v1.0 " E4_CONFIG;

#ifndef FORTH_CORE
  #define FORTH_CORE        1          ///< core to pin Forth to
#endif
#ifndef FORTH_TASK_STACK
  #define FORTH_TASK_STACK  8192       ///< bytes (not words) in ESP-IDF
#endif
#ifndef FORTH_TASK_PRIO
  #define FORTH_TASK_PRIO   5
#endif

///====================================================================
///
///> Memory statistics - for heap, stack, external memory debugging
///
void mem_stat()                    { LOG("%s\n", APP_VERSION); }
void forth_include(const char *fn) {}

static void forth_out(int len, const char *str) {
    fwrite(str, 1, len, stdout);
    fflush(stdout);
}

static void read_line(std::string &line) {
    line.clear();
    for (;;) {
        int c = getchar();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\r' || c == '\n') { putchar('\n'); fflush(stdout); return; }
        if (c == 0x08 || c == 0x7f) {
            if (!line.empty()) { line.pop_back(); fputs("\b \b", stdout); fflush(stdout); }
            continue;
        }
        if (c >= 0x20 && line.length() < E4_IBUF_SZ - 1) {
            line.push_back((char)c);
            putchar(c); fflush(stdout);
        }
    }
}

static void forth_task(void *arg) {
    setvbuf(stdout, NULL, _IONBF, 0);
    forth_init();                                   // init on the core that will run it
    mem_stat();
    srand((int)time(0));

    std::string line;
    for (;;) {
        read_line(line);
        if (forth_vm(line.c_str(), forth_out)) break;   // nonzero => BYE
    }
    forth_teardown();
    LOG("%s Done!\n", APP_VERSION);
    vTaskDelete(NULL);                              // a task must never just return
}

extern "C" void app_main(void) {
#if CONFIG_FREERTOS_UNICORE
    const BaseType_t core = tskNO_AFFINITY;         // single-core chips (S2, C3...)
#else
    const BaseType_t core = FORTH_CORE;
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(
        forth_task, "forth", FORTH_TASK_STACK, NULL, FORTH_TASK_PRIO, NULL, core);
    
    if (ok != pdPASS) printf("[ERROR] forth task create failed\n");
    // app_main returns; the main task is deleted and the system keeps running
}
