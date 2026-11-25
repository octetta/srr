# SRR: Synchronous Message Passing for Portable Concurrent Systems

SRR provides a synchronous **Send / Receive / Reply** service communication model for C99 programs, operating uniformly across in-process threads, local inter-process communication (IPC), serial links, and resource-constrained microcontrollers.

---

## Key Highlights

- **Predictable Control-Plane Transactions:** A client initiates a transaction with `srr_send()` (or `srr_send_timed()`) and blocks until the server answers with `srr_reply()` or a terminal state occurs (timeout, disconnect, channel closure).
- **Single-Winner Resolution:** Concurrent race paths between replies, timeouts, peer disconnections, and channel shutdown are strictly synchronized—a transaction never completes twice.
- **Three-Tier Architecture:** Complete separation between transaction state logic, wire framing/serialization, and platform execution backends.
- **Zero-Heap Embedded Profiles:** Dedicated profiles for microcontrollers (RP2040 Pico, AVR ATmega328P, ESP32, and Parallax Propeller P1) operating entirely on caller-provided static buffers without dynamic heap allocation.
- **Empirical Performance:** Median round-trip latency of **6.7 µs** in-process, up to **446,000 transactions/sec** on multi-threaded workloads, and **2.78 Million frames/sec** COBS framing throughput.
- **Zero Leaks & Race-Free:** 100% verified across 12 test suites under Valgrind with zero memory leaks and zero errors.

---

## Architectural Boundaries

```text
+-------------------------------------------------------------------------------+
|                               1. SRR Core Layer                               |
|   - Channels, Connections, Requests, Replies, and Pulses                      |
|   - Single-winner transaction state machine & lifetime management             |
|   - Bounded queues and backpressure signaling (SRR_QUEUE_FULL)                |
+-------------------------------------------------------------------------------+
                                        |
       +--------------------------------+-------------------------------+
       |                                                                |
+------------------------------------+  +-----------------------------------------------+
|     2. Execution Backends          |  |         3. Wire & Framing Layer               |
|  - POSIX Threads (pthreads)        |  |  - COBS byte stuffing (0x00 delimiter)        |
|  - POSIX IPC (Unix Domain Sockets) |  |  - CRC-16-CCITT integrity verification        |
|  - Windows IPC (Named Pipes)       |  |  - Explicit Little-Endian field packing       |
|  - Embedded Cooperative Engine     |  |  - Streaming chunk parser (zero heap alloc)   |
|  - AVR Tiny Profile (ATmega328P)   |  +-----------------------------------------------+
|  - ESP32 FreeRTOS Task Adapter     |                          |
|  - Parallax P1 Multi-Cog Hub RAM   |                  Physical Transports
+------------------------------------+          (Serial UART, UDP Datagram, USB CDC)
```

---

## Embedded Profile Matrix

| Profile | Target Platforms | Concurrency Model | Dynamic Heap | Static RAM Footprint | Code Size (.text) | Max Payload |
|---|---|---|:---:|:---:|:---:|:---:|
| **Full** | Linux, macOS, Windows | `pthread` / Win32 | Optional (at init) | ~14 KB | ~18 KB | Configurable |
| **Compact** | RP2040 Raspberry Pi Pico | Cooperative main-loop | **Zero** | ~2 KB | ~4.1 KB | 512 B |
| **Tiny** | AVR ATmega328P (Arduino) | Cooperative main-loop | **Zero** | **~392 Bytes** | **~2.8 KB** | 32 B |
| **RTOS** | ESP32 (ESP-IDF) | FreeRTOS tasks/mutexes | Task Stack | ~8 KB | ~2.7 KB | Configurable |
| **Multi-Cog**| Parallax Propeller P1 | 8-Cog Hub RAM mailbox | **Zero** | **~1.1 KB** | **~2.5 KB** | 64 B |

---

## Performance Benchmarks (-O3, Linux x86_64)

Empirical measurements gathered from `make bench` (`benchmarks/bench_srr.c`):

| Benchmark Scenario | Payload / Configuration | Throughput | Median (p50) | 95th Percentile | 99th Percentile |
|---|---|:---:|:---:|:---:|:---:|
| **Pthreads In-Process** | 1 Client -> 1 Server (64B) | **142,407 ops/s** | **6.73 µs** | 7.75 µs | 14.95 µs |
| **Pthreads Multi-Threaded** | 4 Clients -> 4 Servers (64B) | **446,213 ops/s** | **7.65 µs** | 15.83 µs | 23.07 µs |
| **Unix Domain Sockets (IPC)**| 1 Client -> 1 Server (64B) | **54,591 ops/s** | **16.99 µs** | 24.45 µs | 31.84 µs |
| **Queue-Full Rejection** | Saturated Queue (Capacity 32) | **12,938,562 ops/s** | **0.04 µs (40 ns)**| 0.08 µs | 0.32 µs |
| **Wire Frame Encoding** | Header (32B) + Payload (64B) + CRC | **2,783,205 frames/s**| **0.34 µs (340 ns)**| 0.34 µs | 0.36 µs |
| **Wire Streaming Parser** | 1-Byte Chunk Incremental Feed | **2,004,487 frames/s**| **0.47 µs (470 ns)**| 0.48 µs | 1.12 µs |
| **Serial Full-Duplex Link** | 32B Payload End-to-End | **9,840 ops/s** | **94.94 µs** | 153.30 µs | 209.51 µs |

---

## Quickstart Examples

### 1. In-Process & Cross-Process IPC (POSIX & Windows)

```c
#include <stdio.h>
#include <string.h>
#include "srr/srr.h"

/* Server */
void run_server(void) {
    srr_channel_t *chan = NULL;
    srr_channel_create_service("echo_service", NULL, &chan);

    srr_request_t *req = NULL;
    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            printf("Received pulse: code=%u val=%lu\n",
                   srr_pulse_code(req), (unsigned long)srr_pulse_value(req));
            srr_request_dispose(req);
            continue;
        }

        const char *data = (const char *)srr_request_data(req);
        char reply[128];
        snprintf(reply, sizeof(reply), "ACK: %s", data);
        srr_reply(req, reply, strlen(reply) + 1);
    }
    srr_channel_destroy(chan);
}

/* Client */
void run_client(void) {
    srr_connection_t *conn = NULL;
    srr_connect("echo_service", NULL, &conn);

    char reply[128];
    size_t reply_len = 0;
    srr_status_t st = srr_send_timed(conn, "HELLO", 6, reply, sizeof(reply), &reply_len, 1000);
    if (st == SRR_OK) {
        printf("Server replied: %s\n", reply);
    }

    /* Fire-and-forget asynchronous notification */
    srr_pulse(conn, 1, 100);

    srr_disconnect(conn);
}
```

### 2. Zero-Heap Microcontroller Endpoint (RP2040 Pico / Bare Metal)

```c
#include "srr_embedded.h"

static int device_info_handler(uint16_t svc, uint16_t op, const uint8_t *req, size_t req_len,
                               uint8_t *reply, size_t cap, size_t *out_len, void *ud) {
    (void)svc; (void)op; (void)req; (void)req_len; (void)ud;
    const char *info = "RP2040-PICO-1.0";
    size_t len = strlen(info);
    if (cap < len) return -1;
    memcpy(reply, info, len);
    *out_len = len;
    return 0;
}

int main(void) {
    /* Statically allocated buffers - zero dynamic memory */
    static uint8_t rx[512], dec[512], tx[512], rep[512];
    srr_endpoint_t ep;

    srr_endpoint_init(&ep, 0x1001, uart_write_bytes, NULL,
                      rx, sizeof(rx), dec, sizeof(dec),
                      tx, sizeof(tx), rep, sizeof(rep));

    srr_endpoint_register_service(&ep, 1, device_info_handler, NULL);

    while (1) {
        int b = uart_read_byte();
        if (b >= 0) {
            srr_endpoint_feed_byte(&ep, (uint8_t)b);
        }
    }
}
```

### 3. BEAM Ecosystem (Erlang, Elixir, Gleam)

```erlang
%% Erlang: Synchronous GenServer call & async pulse
{ok, Client} = srr_client:start_link("srr_synth").
{ok, Reply} = srr_client:call(Client, <<"STATUS">>).
ok = srr_client:pulse(Client, 1, 16).
```

```elixir
# Elixir: Supervised GenServer client
{:ok, reply} = SRR.call(client, "SET cutoff 1750.0\0")
:ok = SRR.pulse(client, 1, 16)
```

```gleam
// Gleam: Type-safe BitArray wire encoding or Erlang FFI
let assert Ok(client) = srr.connect("srr_synth")
let assert Ok(reply) = srr.call(client, <<"STATUS":utf8, 0:8>>)
let _ = srr.pulse(client, 1, 16)
```

---

## Command-Line Tool (`tools/srr_cli`)

SRR includes a developer CLI for querying, benchmarking, and interacting with endpoints:

```bash
# Query device information from simulated Pico endpoint
./tools/srr_cli --sim info

# Ping simulated microcontroller endpoint
./tools/srr_cli --sim ping

# Send synchronous request over IPC to synthesizer server
./tools/srr_cli --ipc srr_synth req 0 0 "SET cutoff 2200.0"

# Send asynchronous pulse over physical serial port
./tools/srr_cli --serial /dev/ttyACM0 --baud 115200 pulse 10 1 ALARM
```

---

## Language Adapters & Multi-Language Ecosystem

In addition to the C library (`libsrr.a`, `libsrr.so`), SRR provides first-class, standalone language-native libraries under `adapters/`. Each adapter is written strictly with standard libraries—requiring **zero third-party dependencies** and zero C compiler toolchains:

| Language | Directory | Key Facilities | Transports | Status |
|---|---|---|---|:---:|
| **Go** | [`adapters/go/`](adapters/go/) | Client & Server, Goroutine pools | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **Python** | [`adapters/python/`](adapters/python/) | Synchronous client, Resolver, Context managers | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **Node.js** | [`adapters/node/`](adapters/node/) | Promise/async API, zero npm dependencies | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **Ruby** | [`adapters/ruby/`](adapters/ruby/) | Pure Ruby client, zero gems | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **Perl** | [`adapters/perl/`](adapters/perl/) | Pure Perl client, zero CPAN | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **PHP** | [`adapters/php/`](adapters/php/) | Pure PHP client, zero PECL | IPC (Unix Sockets), UDP, Pulses | Native Standard Library |
| **Erlang (BEAM)** | [`adapters/beam/`](adapters/beam/) | OTP `gen_server`, Port Driver | IPC, Stdio Packet Bridge | Native OTP |
| **Elixir** | [`adapters/elixir/`](adapters/elixir/) | Mix package, `GenServer` client | IPC (Unix Sockets), Pulses | Native BEAM |
| **Gleam** | [`adapters/gleam/`](adapters/gleam/) | Bitstring wire framing, type-safe RPC | IPC (Unix Sockets), Pulses | Native BEAM |

See the [`examples/go/`](examples/go/) directory for runnable Go client and microservice server implementations.

---

## Building and Verification

```bash
# Build static library libsrr.a, examples, CLI tools, and BEAM port driver
make all

# Run all 12 native unit and integration test suites
make test

# Run all 13 test suites under Valgrind (0 errors, 0 memory leaks check)
make test-valgrind

# Run cross-language protocol conformance suite
make test-conformance

# Run AddressSanitizer and UndefinedBehaviorSanitizer
make test-asan

# Run ThreadSanitizer
make test-tsan

# Run BEAM (Erlang, Elixir, Gleam) integration tests
make test-beam

# Run full adapter suite (BEAM + WebAudio browser gateway + native adapters)
make test-adapters

# Execute complete verification suite
make test-all

# Run nanosecond microbenchmark suite with -O3
make bench

# Cross-compile Windows library (libsrr_win32.a) and executables via MinGW
make win32
```

---

## Documentation Index

- **[Correctness & Concurrency Audit Report](docs/CORRECTNESS_AUDIT.md):** Detailed analysis of resolved defects, concurrency state arbitration, lifetime rules, regression tests, and operational risk assessment.
- **[Protocol Conformance Specification](docs/PROTOCOL_CONFORMANCE.md):** Formal specification distinguishing 24-byte IPC streaming framing from 32-byte Device Wire framing, byte ordering, CRC-16, COBS, and cross-language conformance matrix.
- **[Platform Verification & Resource Report](docs/PLATFORM_VERIFICATION.md):** Truthful resource budgets, compiler reports, and distinction between host emulation and physical target hardware for RP2040 Pico, ESP32, AVR, and Parallax P1.
- **[Emulators, Virtual Targets & FFI Guide](docs/EMULATORS_AND_VIRTUAL_TARGETS.md):** How to run AVR (simavr), Pico, ESP32, and Parallax P1 (Spin/spinsim) on Fedora without physical hardware, plus scripting FFI guide (`libsrr.so`).
- **[Composing Distributed Systems with SRR](docs/COMPOSING_SYSTEMS_WITH_SRR.md):** Complete architectural guide on system topologies, dynamic resolvers, mesh federation, and real-world case studies (synthesizers, robotics, industrial control).
- **[BEAM Inter-Op Guide (Erlang / Elixir / Gleam)](adapters/beam/README.md):** Native Unix Domain Socket `gen_server`, port drivers, Elixir Hex package, and Gleam bitstring framing.
- **[Scripting & Automation Guide](docs/SCRIPTING_AND_AUTOMATION.md):** Integration guide and examples for Bash, Python, Perl, Tcl, PHP, PowerShell, and Gforth.
- **[Getting Started Guide](docs/GETTING_STARTED.md):** Introductory tutorial covering threads, IPC, BEAM, and WebAudio.
- **[C API Reference](docs/API_REFERENCE.md):** Exhaustive function reference for Core, Wire, Serial, and Embedded APIs.
- **[Architecture Specification](docs/ARCHITECTURE.md):** Detailed state machine and transport engineering guide.
- **[Performance & Benchmark Report](docs/SRR_BENCHMARKS.md):** Detailed latency distributions, throughput tables, and memory analysis.
- **[Embedded Profiles Specification](docs/SRR_EMBEDDED_PROFILES.md):** Detailed resource budgets and build guides for AVR Tiny and ESP32.
- **[Parallax Propeller P1 Multi-Cog Architecture](docs/SRR_PROPELLER_P1.md):** Shared Hub RAM and Cog mailbox arbitration design.
- **[Wire Protocol Specification](docs/SRR_WIRE_PROTOCOL.md):** Binary framing, COBS encoding, and CRC-16-CCITT algorithm definition.
- **[Formal API Specification](docs/SRR_API_SPEC.md):** Behavioral contracts, ownership semantics, and concurrency rules.
- **[Architectural Decisions Log](docs/SRR_DECISIONS.md):** Key design choices, trade-offs, and resolutions.
- **[Test Plan & Verification Matrix](docs/SRR_TEST_PLAN.md):** Concurrency, race, fuzzing, and fault-injection matrix.
- **[Implementation Roadmap](docs/SRR_IMPLEMENTATION_PLAN.md):** Phased milestone breakdown across stages.
