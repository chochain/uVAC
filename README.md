# uVAC: UNIVAC 1108 Retro on ESP32-S3

A micro-UNIVAC for your desk: a multi-user, EXEC 8-flavoured Forth system on a $10 ESP32.

This project explores the architectural elegance of 1960s mainframe computing by mapping historical **EXEC 8** Operating System concepts directly onto modern **FreeRTOS** symmetric multiprocessing (SMP), an asynchronous **Actor model** network, and a interactive resident **Forth Core** acting as the primary Operating System interface.

Open a browser tab and you get your own private Forth session. Open another tab and you get another one. Every tab has its own data stack and its own place in the code, while all of them share one dictionary, one memory image and one tiny machine, the way users of a 1960s mainframe shared one big one.

<picture>
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/chochain/uvac/blob/main/docs/img/uvac_sesx4_snip01.png" width="800" />
</picture>

---

## Why uVAC?

### The idea?

The UNIVAC 1108 and its operating system, **EXEC 8**, are an early example of a machine that did several things for several people at once. Batch jobs, interactive "demand" users at terminals, and real-time work all ran under one executive. Users never saw each other, yet they shared the same CPU, memory, files and system software.

An ESP32 today has more raw compute than the room-sized machines of that era had, and it comes with WiFi and a few megabytes of RAM. **Forth** is the other half of the idea. Forth is small, interactive and extensible, and it has always been a natural fit for "one little machine, many things going on."

uVAC puts the two together:

> *What if a microcontroller behaved like a tiny time-sharing mainframe, with each user a browser tab and the "terminal" the web?*

### Unfinished business: Forth on the UNIVAC 1108 (1970)

uVAC is more than a coincidence of names. Forth's inventor, Chuck Moore, actually built a Forth for a UNIVAC 1108.

According to the history of Forth by Rather, Colburn and Moore, in 1970 Moore was assigned by Mohasco Industries to an ambitious project: a new UNIVAC 1108 handling a network of leased lines for an order-entry system. He ported Forth onto the 1108, coded in assembler, and interfaced it to COBOL modules that did the transaction processing. That Forth buffered input and output messages and **shared the CPU among tasks, one for each line**.

That is a multi-user, multi-terminal Forth executive on a UNIVAC. The project was cancelled by Mohasco in an economic downturn before it was completed, and Moore left shortly afterwards. It was not a failure of the idea, only of the circumstances.

uVAC intents to pick up that thread. The leased lines become browser tabs, the shared CPU becomes the cooperative tasker, and the COBOL transaction modules become whatever words you define. In effect, it is the "tasks handling each line" design of 1970, finally running on hardware small enough to hold in your hand. See the [References](#references).

### Design principles

1. **One machine, many users.** Sessions are cheap. A tab is a user.
2. **Private state, shared system.** Each session keeps its own stacks and cursor. The dictionary, code and memory are common, so one user can define a word and another can call it.
3. **The executive stays in control.** A session is given a slice of the CPU and hands it back. No user can hog the machine.
4. **Keep it small and legible.** Forth, a handful of C++ files, and an actor loop that can fit in one's head.
5. **Retro in spirit, modern in plumbing.** The mental model is 1965, but the transport is HTTP, chunked streaming and HTMX.

---

## Features

### Multi-user, browser-based sessions
- Each **browser tab** is a separate user with its own persistent Forth VM. The tab identity is a random per-tab id (`sessionStorage`) combined with a random 128-bit cookie issued by the device.
- A tab's **data stack survives between requests**, so `123 456` in one input and `+ .` in the next line works, as in a real terminal session. Reloading the page keeps the same session.
- **Streaming output.** Output goes back over a chunked HTTP response as it is produced, so long-running words show their output live.
- **ABORT button** per tab: stops that tab's running word and cleans its control state without touching other users.
- A pool of **16 VM slots** (slot 0 is the system shell). When the pool is full the least-recently-used idle tab is evicted.

### An executive instead of a free-for-all
- A **cooperative tasker**: a session runs a *slice*, then pauses, sleeps or finishes, and the next one gets the engine.
- `pause`, `ms` and loops that yield let long computations share the machine.
- A single **engine lock** serialises slices. The dictionary, code memory and I/O buffers are shared, so only one slice touches them at a time.
- **Compiler ownership:** while one user has an open `:` definition, other users can still run words but not start their own definition. This avoids the problem of two users compiling into one dictionary at the same time.
- Background **tasks** (`task`, `start`) run alongside the sessions.

### Actor architecture
- A small **actor system** with a message queue and a pool of worker threads on core 0 decouples the web, Forth and GUI parts.
- Control messages (`EXEC`, `RESUME`, `EOL`, `DONE`, `ABORT`) travel through the queue. Output travels out-of-band, directly to the session's socket, so lines always arrive in order.
- Any worker can run any actor, and per-session state lives in the VM, not in the actor.

### Forth core
- A direct-threaded eForth with 32-bit cells (integer build by default, floating point optional).
- A **trampoline** dispatch mode for the Xtensa windowed ABI used by the ESP32-S3, with a tail-call mode for ARM, RISC-V and x86. This is selected at compile time.
- Interrupted words (via `pause`) are resumable at the instruction level.

### Hardware niceties
- Input buffers live in **PSRAM** (octal PSRAM on ESP32-S3 boards).
- An optional **LVGL-based display** actor mirrors session output on a local screen, prefixed with the task id. The code ships with an XGL renderer at 480×480.

---

## Architecture

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

### State Diagram
<picture>
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/chochain/uvac/blob/main/docs/img/uvac_request_to_response_states.png" width="800" />
  <img alt="State Diagram" src=""https://github.com/chochain/uvac/blob/main/docs/img/uvac_request_to_response_states.png">
</picture>

One request's life: the `POST` body lands in PSRAM, the tab's VM is attached, and the coordinator feeds the input **one line at a time** to the Forth actor. Each line runs as one or more slices. When a line ends, the next one is dispatched. When the input is exhausted, the response is closed and the VM is released, with its **stack kept** for the next request.

### Sequence Diagram
<picture>
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/chochain/uvac/blob/main/docs/img/uvac_sequence-with_actors.png" width="800" />
  <img alt="Sequence Diagram" src=""https://github.com/chochain/uvac/blob/main/docs/img/uvac_sequence-with_actors.png">
</picture>

---

## How it compares

uVAC borrows ideas from several worlds. None of these are one-to-one equivalents, but they help locate it.

### Architectural Mappings:
* **EXEC 8 Symbionts**: The `XServer` asynchronous routing engine acts exactly like an 1108 input symbiont. It ingests raw HTTP plain-text network scripts from a custom HTMX web frontend, stream-buffers them straight into **8MB of external Octal PSRAM capsules**, and spools them line-by-line out of the core processor's critical path into the Forth OS environment.
* **The Central Processor (CPU 0)**: Pinned to **Core 0**. Runs the central asynchronous `ActorSystem` worker thread-pool executing the low-level processing engine (`ForthActor`).
* **The Main OS Interface (Forth Core)**: Integrated directly as the underlying execution machine (`forth_vm`). Forth words map low-level hardware configurations (`mcu_actor.h`), state control loops, and dictionary operations, serving as the shell, macro interpreter, and direct supervisor of the mainframe state.
* **Input/Output Controller (IOC)**: Pinned strictly to **Core 1**. Running at a higher FreeRTOS priority, it handles the **ST7701S Parallel RGB line timing** and **GT911 touch telemetry** via the LVGL graphics stack. This prevents screen refreshing overhead from introducing latency into core calculations or Forth word execution.

### EXEC 8 concepts → uVAC

| EXEC 8 / UNIVAC 1108 idea | uVAC counterpart |
|---|---|
| One executive running many users and job types | Actor system + cooperative tasker |
| Demand (interactive) terminal | Browser tab |
| Per-run context | Per-tab Forth VM (stacks, cursor, radix) |
| Shared system files and libraries | Shared dictionary and memory |
| Scheduler hands out CPU quanta | Slices ended by `pause`, `ms` or loop yield |
| Operator can kill a run | ABORT button |
| Multiple processors sharing one executive | Two cores: web and workers on core 0, UI on core 1 |
| Batch input stream | Multi-line input fed to the VM line by line |
| Moore's 1970 Forth: one task per leased line, CPU shared among them | One session VM per browser tab, slices shared by the tasker |

It is a tribute and a teaching model, not an emulation. uVAC does not run 1108 code, and it has no 36-bit words, no Fieldata and no `@RUN` card images.

TODO: Workload Scheduling Modes
uVAC simulates the three historical runtime priorities managed by the classic UNIVAC EXEC 8 kernel using a customized `job_class_t` matrix directed through the Forth OS:

1. **`JOB_BATCH` (Background Processing)**: Submit-and-collect workloads. Code blocks are spooled quietly into background PSRAM queues without holding open interactive communication pipelines.
2. **`JOB_DEMAND` (Time-Sharing Interface)**: Low-latency interactive tasks. This drives the real-time web terminal console interface, giving developers a direct interactive REPL loop into the resident Forth OS.
3. **`JOB_REALTIME` (Preemptive Interruption)**: High-priority execution tokens. Utilizing the custom actor pipeline's `send_priority()` methods, these jobs bypass the normal FIFO dispatch queues to cut straight to the front of the processing stream.

### Versus other ways to get a REPL on an ESP32

| | uVAC | MicroPython / Lua | Espruino | Serial Forth (e.g. Mecrisp) | ESP-IDF console |
|---|---|---|---|---|---|
| Interface | Web tabs | Serial / WebREPL | Serial / web IDE | Serial | Serial |
| Concurrent *interactive* users | Yes, per tab | Usually one | Usually one | One | One |
| Persistent per-user state | Stacks per tab | Global namespace | Global | One stack | None |
| Shared live system | Yes (dictionary) | Yes | Yes | Yes | n/a |
| Footprint and style | Small, Forth | Larger, high-level | Mid, JS | Very small | Command-oriented |

### Versus classic multi-user Forths
Multi-user Forth has a long history, for example the multi-user systems of Forth, Inc. Those typically give each user a *task* with a private user area and share the dictionary. uVAC follows the same pattern, but adds a web transport and an actor-based executive, and runs on a modern microcontroller.

---

## Possible uses

- **A shared lab instrument.** Several people (or several scripts) poke at one device at once. A student can watch a sensor in one tab while a teammate tunes an output in another.
- **Teaching operating-system ideas.** Scheduling, context, shared state, locks and abort semantics fit on one screen. Students can read the whole executive.
- **Teaching Forth.** Each learner gets a private stack, with no serial cable and no installs, just a URL on the classroom WiFi.
- **Home and workshop automation.** Define words like `lights-on` once, and every tab can call them. `pause` and tasks let you write small cooperative control loops.
- **Retro-computing demos and exhibits.** Run a "mainframe" you can carry in your pocket.
- **A base for experiments** in cooperative multitasking, message-passing, and web-native REPLs.

---

## Getting started

> The sketch targets ESP32-S3 boards with PSRAM and the Arduino-ESP32 core. Other ESP32 variants may need changes in `config.h` and the PSRAM allocations.

1. Clone the repository.
2. Put your WiFi credentials in the sketch (see **Keep your credentials out of Git** below).
3. Select your ESP32-S3 board with PSRAM enabled, then build and upload `uvac.ino`.
4. Open the Serial Monitor at 115200 baud and wait for the line `xserver2> live at http://<ip>`.
5. Browse to that address. Open a second tab for a second user.

Key knobs in `config.h`:

| Setting | Meaning | Default |
|---|---|---|
| `E4_VM_POOL_SZ` | Session and task slots (slot 0 is the shell) | 16 |
| `E4_PMEM_SZ` | Shared Forth memory | 32 KB |
| `E4_DICT_SZ` | Dictionary entries | 400 |
| `USE_FLOAT` | Floating-point cells | 0 |
| `USE_PTHREAD` | Preemptive pthreads or the cooperative tasker | 0 (cooperative) |
| `WORKER_TASK_COUNT` (sketch) | Dispatcher workers; keep at least 2 | 4 |

### Keep your credentials out of Git

Do not commit real WiFi credentials. Keep them in an untracked header and add it to `.gitignore`:

```cpp
// secrets.h  (listed in .gitignore)
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
```

If a password has ever been committed, change it on the router, since Git history keeps old versions.

---

## Try it

```forth
\ a word that yields so other tabs keep running
: count3  3 for i . cr pause next ;

count3                     \ run it in tab A
\ meanwhile, in tab B, run anything: both progress together
```

---

## Current limits

Being clear about what uVAC is not:

- **Not preemptive.** Fairness depends on words reaching a yield point. Long inner loops should yield, and the engine can do so automatically for loops. A tight loop with no yield point can still hold the engine for its slice.
- **Not isolated.** All sessions share one memory and one dictionary. A user can redefine or overwrite things others depend on. This is a trusted-lab model, not a security boundary.
- **No encryption or real authentication.** It uses plain HTTP, and the cookie is only a random session handle. Use it on a private network.
- **One compiler at a time.** An open `:` definition blocks other users from starting their own until it is closed.
- **Small by design.** 16 VM slots, a 32 KB shared memory, and a 1 KB output buffer per flush.

---

## Source map

```
├── uvac.ino                   # Main entry point; boots Actor System & subsystems
├── src/
│   ├── config.h               # eForth build configuration
│   ├── ceforth.h/cpp          # eForth core
│   ├── ceforth_sys.cpp        # eForth IO subsystem
│   ├── ceforth_task.cpp       # eForth multi-tasking subsystem
│   ├── ceforth_coop.cpp       # eForth cooperative subsystem
│   ├── e4_run.S               # Xtensa trampoline loop
│   ├── forth_api.h            # narrow API between actors and the Forth engine
│   └── esp32/
│       ├── uvac.h             # uVAC ESP32 implementation interace
│       ├── xactor.h/cpp       # Central Actor System framework & worker dispatch engines
│       ├── xserver2.h/cpp     # esp_http_server implementation & PSRAM buffering
│       ├── xsvr_actor.h       # session coordinator, line feeder, response closer
│       ├── xforth_actor.h     # Wrapper executing the primary Forth OS VM & routing logs
│       └── xgl_actor.h/cpp    # LVGL framework initialization & parallel RGB panel configs
```

---

## TODOs - Roadmap ideas

- Per-user dictionaries or word ownership.
- A **session console**: `who`, `kill`, `ps`-style words (an operator's view, in the spirit of an executive console).
- File storage (LittleFS) with simple per-user areas and `@ASG`-like naming.
- Resource accounting per session, such as CPU slices used.
- Real-time tasks and timed batch jobs queued from the web.
- Serial and websocket front ends beside the HTTP one.

---

## References

- Elizabeth D. Rather, Donald R. Colburn, Charles H. Moore, *The Evolution of Forth* (ACM HOPL-II, 1993). Section 1.1, "Early Development", covers the Mohasco UNIVAC 1108 project. Online at <https://www.forth.com/resources/forth-programming-language/>.
- Charles H. Moore, *Forth - The Early Years* (1991). <https://colorforth.github.io/HOPL.html>.
- Charles H. Moore, *FORTH: A Language for Interactive Computing* (1970), the first paper on Forth.
- Charles H. Moore, "The Evolution of FORTH, an Unusual Language", *BYTE*, August 1980.
- *Forth Dimensions* Vol. 1 No. 6, where Moore also recalls porting Forth to the 1108.

The 1970 1108 Forth is described only in prose in these sources, and no known source code or manual survives that I am aware of. uVAC does not claim to reproduce it, only to continue the idea.

## Acknowledgements

- **eForth** and the Forth community, for a language that fits in your head and on a microcontroller.
- **Chuck Moore**, for Forth, and for the 1970 UNIVAC 1108 system that this project hopes to carry on.
- **UNIVAC 1108 / EXEC 8**, and the engineers who made a multi-user machine out of 1960s hardware, for the inspiration.
- **ESP-IDF**, **Arduino-ESP32**, **HTMX** and **LVGL**.

UNIVAC and EXEC 8 are historical products of their respective trademark holders. uVAC is an independent hobby project with no affiliation.

## License

MIT license
