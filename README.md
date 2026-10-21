# uVAC: UNIVAC 1108 Retro on ESP32-S3

A dual-core, hardware-accelerated **UNIVAC 1108 architecture simulator and educational platform** built for the **ESP32-S3** (specifically targeting the 4848S040 ST7701S smart display board). 

This project explores the architectural elegance of 1960s mainframe computing by mapping historical **EXEC 8 Operating System concepts** directly onto modern **FreeRTOS symmetric multiprocessing (SMP)**, an asynchronous **Actor model network**, and a interactive resident **Forth Core acting as the primary Operating System interface**.

---

## 🏛️ The Mainframe Architecture Analogy

In the 1960s, the **UNIVAC 1108** triumphed over slow external peripherals (such as punch card readers and magnetic tape drives) using **Symbiont routines**—specialized asynchronous spooling programs that isolated slow I/O operations from the central computational unit. 

**uVAC** models this exact hardware topology across the ESP32-S3's dual-core Xtensa processor, utilizing a **Forth core** as the resident OS supervisor:

```
                  ┌───────────────────────────────────────┐
                  │          WEB INTERFACE (HTML5/HTMX)   │
                  └───────────────────┬───────────────────┘
                                      │ (Wi-Fi Payload)
                                      ▼
    ┌──────────────────────────────────────────────────────────────────────────┐
    │ CORE 0: CENTRAL MAINFRAME ENGINE                                         │
    │                                                                          │
    │  ┌───────────────────────┐             ┌──────────────────────────────┐  │
    │  │  XServer (Symbiont)   │ ──[Alloc]──>│ 8MB Octal PSRAM Memory Bank  │  │
    │  └───────────────────────┘             └──────────────────────────────┘  │
    │              │                                  │                        │
    │              │ (Parsed Card Streams)            │ (Capsule Data)         │
    │              ▼                                  ▼                        │
    │  ┌────────────────────────────────────────────────────────────────────┐  │
    │  │  ForthActor Engine (Main OS & Central Processing Unit / forth_vm)  │  │
    │  └────────────────────────────────────────────────────────────────────┘  │
    └──────────────────────────────────┬───────────────────────────────────────┘
                                       │ (Async Inter-Core Mailbox Msg)
                                       ▼
    ┌──────────────────────────────────────────────────────────────────────────┐
    │ CORE 1: INPUT/OUTPUT CONTROLLER (IOC)                                    │
    │                                                                          │
    │  ┌────────────────────────────────────────────────────────────────────┐  │
    │  │  XGL Renderer Actor (LVGL v8 / High-Speed DMA / ST7701S RGB Panel) │  │
    │  └────────────────────────────────────────────────────────────────────┘  │
    │              │ (9MHz Bus Optimization)                                   │
    │              ▼                                                           │
    │  [480x480 Smart Display Console] <──[Hardware Touch Engine (GT911)]      │
    └──────────────────────────────────┴───────────────────────────────────────┘
```

### Architectural Mappings:
* **EXEC 8 Symbionts**: The `XServer` asynchronous routing engine acts exactly like an 1108 input symbiont. It ingests raw HTTP plain-text network scripts from a custom HTMX web frontend, stream-buffers them straight into **8MB of external Octal PSRAM capsules**, and spools them line-by-line out of the core processor's critical path into the Forth OS environment.
* **The Central Processor (CPU 0)**: Pinned to **Core 0**. Runs the central asynchronous `ActorSystem` worker thread-pool executing the low-level processing engine (`ForthActor`).
* **The Main OS Interface (Forth Core)**: Integrated directly as the underlying execution machine (`forth_vm`). Forth words map low-level hardware configurations (`mcu_actor.h`), state control loops, and dictionary operations, serving as the shell, macro interpreter, and direct supervisor of the mainframe state.
* **Input/Output Controller (IOC)**: Pinned strictly to **Core 1**. Running at a higher FreeRTOS priority, it handles the **ST7701S Parallel RGB line timing** and **GT911 touch telemetry** via the LVGL graphics stack. This prevents screen refreshing overhead from introducing latency into core calculations or Forth word execution.

---

## ⚡ EXEC 8 Workload Scheduling Modes

uVAC simulates the three historical runtime priorities managed by the classic UNIVAC EXEC 8 kernel using a customized `job_class_t` matrix directed through the Forth OS:

1. **`JOB_BATCH` (Background Processing)**: Submit-and-collect workloads. Code blocks are spooled quietly into background PSRAM queues without holding open interactive communication pipelines.
2. **`JOB_DEMAND` (Time-Sharing Interface)**: Low-latency interactive tasks. This drives the real-time web terminal console interface, giving developers a direct interactive REPL loop into the resident Forth OS.
3. **`JOB_REALTIME` (Preemptive Interruption)**: High-priority execution tokens. Utilizing the custom actor pipeline's `send_priority()` methods, these jobs bypass the normal FIFO dispatch queues to cut straight to the front of the processing stream.

---

## 📁 Codebase Directory Structure

```
├── uVAC.ino                   # Main entry point; boots Actor System & subsystems
├── main/
│   ├── config.h               # eForth build configuration
│   ├── ceforth.h/cpp          # eForth core
│   ├── ceforth_sys.cpp        # eForth IO subsystem
│   ├── ceforth_task.cpp       # eForth multi-tasking subsystem
│   └── esp32/
│       ├── uvac.h             # uVAC ESP32 implementation interace
│       ├── xactor.h/cpp       # Central Actor System framework & worker dispatch engines
│       ├── xque.h             # Native FreeRTOS vs. Desktop STL fallback Mailbox queues
│       ├── xbridge.h          # Shared mainframe workload typedefs & wire structures
│       ├── xserver_actor.h    # Chunked HTTP streaming sessions driven by Actors
│       ├── xserver_mux.h      # Chunked HTTP streaming sessions handshake multiplexer
│       ├── xserver2.h/cpp     # esp_http_server implementation & PSRAM buffering
│       ├── xforth_actor.h     # Wrapper executing the primary Forth OS VM & routing logs
│       └── xgl_actor.h/cpp    # LVGL framework initialization & parallel RGB panel configs
```

---

## 🚀 Performance Optimizations for ESP32-S3

* **PSRAM Bus Bandwidth Throttling**: The parallel RGB panel driver is explicitly configured with a **9MHz pixel clock reduction** (down from 18MHz). This optimization clears up memory bus contention, giving Core 0 high-speed access to the external Octal SPI RAM capsules for seamless Forth execution.
* **Zero-Wait Interrupt Handling**: In-flight timeouts or runtime cancellation signals (`MSG_FORTH_ABORT`) bypass processing queues entirely via zero-wait non-blocking priority mechanisms, mimicking hardware line interrupts to force an immediate Forth VM reset.
* **Network-to-Display Bypass**: Output logs stream instantly across a dedicated Inter-Core Actor connection straight from Core 0 to the XGL Display Actor on Core 1 (`MSG_GUI_DRAW_CMD`), bypassing intermediate network buffers so your screen echoes Forth evaluations in real-time.

---

## 🛠️ Quick Start & Usage

### Control Card Scripting Syntax
You can control the mainframe engine routing manually from the web terminal using control cards that wrap standard Forth words:

```forth
\ 1. Submit a Real-time Preemptive Execution Job
@REALTIME
: ALARM-LOOP 5 0 DO ." [IRQ OVERRIDE] " CR LOOP ; ALARM-LOOP

\ 2. Submit a Long-running Background Compute Task
@BATCH
: CALC-MATRIX 10000 0 DO I . LOOP ; CALC-MATRIX
```

### Compilation Requirements
1. **Hardware**: ESP32-S3 4848S040 Board (4.0" 480x480 Smart Display with Octal PSRAM).
2. **Libraries**: `Arduino_GFX_Library`, `TAMC_GT911`, `lvgl` (v8.x recommended).
3. **Configuration**: Set your target router credentials inside `uVAC.ino` before launching the compilation chain.
