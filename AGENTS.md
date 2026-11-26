# AGENTS.md: The Definitive Autonomous Agent Reference for SRR

> **Target Audience:** Autonomous AI Coding Agents, Automated Tooling, and Systems Engineers.  
> **Repository Version:** `v0.1.1` | **Wire Protocol Magic:** `0x53525231` ("SRR1") | **Protocol Version:** `1`  
> **Core Purpose:** Enable agents to generate correct, zero-hallucination, idiomatic SRR code across all supported hardware and software tiers without prior context.

---

## 1. System Identity: What is SRR?

**SRR (Send-Receive-Reply)** is an ultra-lightweight, deterministic, discrete-message-oriented IPC and RPC engine inspired by the classic **QNX microkernel**.

### What SRR Is:
* **Strictly Request-Reply:** A client issues `srr_send()` and blocks until the server calls `srr_reply()`.
* **Zero-Allocation Ready:** Can run completely heapless (zero `malloc`) on 8-bit microcontrollers with as little as 335 bytes of RAM.
* **Message-Oriented (Never Stream-Oriented):** Delivers exact discrete frames. Applications never loop on `recv()` or parse delimiters.
* **Hardware-Bound Flow Control:** The client is suspended while the server processes. Backpressure is innate; servers cannot be flooded.
* **Stale-Reply Safe:** If a client times out or disconnects, any late reply produced by the server is safely intercepted and discarded by the state machine. Stale replies can never corrupt subsequent transactions.

### What SRR is NOT (Non-Goals):
* **NOT a Pub/Sub Broker:** Do not use SRR for 1-to-N fan-out event streams (use MQTT/Kafka/Zenoh).
* **NOT a Bulk Streamer:** Do not stream raw 4K video or multi-gigabyte files through SRR (use raw TCP, WebRTC, or DMA).
* **NOT an Asynchronous Message Queue:** Do not treat SRR like RabbitMQ or Celery.

---

## 2. The 3 Core Mental Shifts & The Golden Rules

Agents generating SRR code **must strictly adhere** to these three invariants:

```
+---------------------------------------------------------------------------------------------------+
| THE GOLDEN RULE OF SRR SERVERS:                                                                   |
| For every transaction received via srr_receive(), the server MUST perform EXACTLY ONE action:     |
|                                                                                                   |
|   1. Normal Request  --> Call srr_reply() OR srr_reply_error()                                    |
|   2. Pulse (is_pulse)--> Call srr_request_dispose() (NEVER call srr_reply on a pulse!)            |
|   3. Discard/Drop    --> Call srr_request_dispose()                                               |
|                                                                                                   |
| Failure to answer or dispose leaves the client thread/worker PERMANENTLY BLOCKED.                 |
+---------------------------------------------------------------------------------------------------+
```

### The 3 Core Mental Shifts
1. **Inversion of Control (Clients Wait on Servers):** In socket programming, servers chase clients. In SRR, the server sits in a single blocking `srr_receive()` loop. Clients line up; the server dictates processing speed.
2. **State Machine as Lock:** Mutexes protect the queue, but the SRR state machine (`SEND_BLOCKED` $\to$ `REPLY_BLOCKED`) arbitrates concurrency. Applications never write complex lock hierarchies.
3. **Discrete Messages, Not Streams:** Never parse `\n` or length headers in your application. One `srr_send` produces exactly one `srr_receive`.

### Strict Agent Anti-Pattern Matrix
| NEVER DO THIS (Halts / Bugs) | ALWAYS DO THIS (Idiomatic SRR) |
| :--- | :--- |
| `while (srr_receive(chan, &req) == SRR_OK) { free(req); }` | Always call `srr_reply(req, ...)` or `srr_request_dispose(req)`. |
| Calling `srr_reply()` on a pulse request (`srr_request_is_pulse(req) == true`). | Check `srr_request_is_pulse(req)` first; call `srr_request_dispose(req)`. |
| Allocating buffers with `malloc()` inside the request loop on embedded tiers. | Use static buffers or stack `union` overlays (`server_msg_union_t`). |
| Parsing byte streams with `read()` / `recv()` loops. | Rely on SRR's framing engine; receive complete frames directly. |
| Ignoring return codes on `srr_send()` or `srr_send_timed()`. | Check for `SRR_OK`, `SRR_TIMEOUT`, or `SRR_SERVER_DIED`. |

---

## 3. Effective Maximum Payload Sizes Across All Tiers

Agents must size buffers according to the physical transport being targeted:

| Tier / Target | Max Payload Size | Default Size | Governing Limit / Constraint |
| :--- | :---: | :---: | :--- |
| **Host OS In-Process & IPC**<br>*(pthreads, Unix Sockets, Win32 Pipes)* | **Configurable (RAM limit)** | **64 KiB** (`SRR_DEFAULT_MAX_MSG_SIZE`) | Configured via `srr_channel_options_t.max_request_size`. Header field is 32-bit (up to 4 GiB). Returns `SRR_TOO_LARGE` if exceeded. |
| **Embedded 32-bit Wire**<br>*(ARM Cortex-M, ESP32, Serial UART)* | **990 bytes** | **990 bytes** | Frame scratch budget (`1024 - 32-byte header - 2-byte CRC-16`) in `srr_wire.c`. |
| **Network RPC over UDP**<br>*(Wi-Fi, Ethernet)* | **1,438 bytes** | **1,438 bytes** | Ethernet MTU (1500) minus IP (20) minus UDP (8) = 1472, minus 32B SRR wire header and 2B CRC-16. |
| **Multi-Core Propeller 1**<br>*(Hub RAM Cog Mailbox)* | **64 bytes** | **64 bytes** | Fixed compile-time mailbox limit (`SRR_P1_MAX_PAYLOAD`) in `srr_p1_hub.h`. |
| **Deep Embedded 8-bit MCU**<br>*(AVR ATmega328P / Arduino)* | **32 bytes** | **32 bytes** | Static buffer limit (`SRR_TINY_MAX_PAYLOAD`) in `srr_tiny.h` to fit inside 2 KB total SRAM. |
| **Native Pulses (`srr_pulse`)** | **0 bytes payload** | **0 bytes** | Carries 32-bit `code` and 64-bit `value` embedded directly inside header. |

---

## 4. Binary Wire Framing Specification

All SRR frames across physical boundaries use packed, little-endian binary headers.

### A. Host IPC Frame Header (24 Bytes - Unix Sockets & Named Pipes)
Defined in `src/srr_proto.h`:
```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* 0x53525231 ("SRR1") */
    uint16_t version;       /* 1 */
    uint16_t msg_type;      /* 1=HELLO, 2=REQUEST, 3=REPLY, 4=ERROR, 5=PULSE, 6=CANCEL, 7=GOODBYE */
    uint32_t flags;         /* Reserved (0) */
    uint64_t trans_id;      /* Transaction correlation identifier */
    uint32_t payload_len;   /* Byte length of application payload following header */
} srr_frame_header_t;
#pragma pack(pop)
```

### B. Device Wire Frame Header (32 Bytes - Serial UART, UDP, Embedded)
Defined in `include/srr/srr_wire.h`:
```c
typedef struct {
    uint32_t magic;         /* 0x53525231 ("SRR1") */
    uint16_t version;       /* 1 */
    uint16_t msg_type;      /* 1=HELLO, 2=REQUEST, 3=REPLY, 4=ERROR, 5=PULSE, 6=CANCEL, 7=GOODBYE */
    uint16_t flags;         /* Reserved (0) */
    uint16_t service_id;    /* Service namespace ID (e.g. 0x0010 = Motor) */
    uint16_t operation_id;  /* Operation ID (e.g. 0x0001 = SetSpeed) */
    uint16_t reserved;      /* Padding (0) */
    uint32_t session_id;    /* Reconnect / reboot detection session */
    uint64_t trans_id;      /* Transaction correlation identifier */
    uint32_t payload_len;   /* Byte length of application payload */
} srr_wire_header_t;
```
* **CRC-16 Algorithm:** CRC-16-CCITT (False): Polynomial `0x1021`, Initial `0xFFFF`, No reflect. Appended directly after payload.
* **COBS Delimitation:** For asynchronous serial streams, frames are COBS-encoded and terminated with a zero delimiter byte (`0x00`).

---

## 5. Platform Implementation Cheatsheet (Copy-Paste Ready)

### Tier 1: In-Process Multithreading (C / pthreads)
```c
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "srr/srr.h"

static void *server_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            srr_request_dispose(req);
            continue;
        }
        const char *in = (const char *)srr_request_data(req);
        char reply[64];
        snprintf(reply, sizeof(reply), "ACK:%s", in);
        srr_reply(req, reply, strlen(reply) + 1);
    }
    return NULL;
}

int main(void) {
    srr_channel_t *chan = NULL;
    srr_channel_create(NULL, &chan);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, chan);

    srr_connection_t *conn = NULL;
    srr_connect_channel(chan, NULL, &conn);

    char reply[64];
    size_t reply_sz = 0;
    srr_status_t rc = srr_send(conn, "PING", 5, reply, sizeof(reply), &reply_sz);
    if (rc == SRR_OK) printf("Reply: %s\n", reply);

    srr_disconnect(conn);
    srr_channel_close(chan);
    pthread_join(th, NULL);
    srr_channel_destroy(chan);
    return 0;
}
```

### Tier 2: Host OS IPC (Linux/macOS Unix Sockets & Windows Named Pipes)
*To move across process boundaries, change only the setup calls:*
```c
// SERVER PROCESS:
srr_channel_t *chan = NULL;
srr_channel_create_service("motor_service", NULL, &chan);
// ... run standard srr_receive() loop ...

// CLIENT PROCESS:
srr_connection_t *conn = NULL;
srr_connect("motor_service", NULL, &conn);
srr_send(conn, req_buf, req_len, rep_buf, sizeof(rep_buf), &rep_len);
srr_disconnect(conn);
```

### Tier 3: Network RPC over UDP (`srr_udp.h`)
```c
#include "srr/srr_udp.h"

// SERVER:
srr_udp_endpoint_t *srv = srr_udp_server_create(8888, NULL);
srr_wire_header_t hdr;
uint8_t payload[1400];
size_t payload_len = 0;
srr_udp_peer_t *peer = NULL;

if (srr_udp_server_receive(srv, &hdr, payload, sizeof(payload), &payload_len, &peer, 5000) == 0) {
    const char *reply = "PONG";
    srr_udp_server_reply(srv, peer, &hdr, reply, strlen(reply) + 1);
    srr_udp_peer_free(peer);
}
srr_udp_close(srv);

// CLIENT:
srr_udp_endpoint_t *cli = srr_udp_client_create("192.168.1.50", 8888, NULL);
char rep[64];
size_t rep_sz = 0;
int rc = srr_udp_request(cli, 1, 1, "PING", 5, rep, sizeof(rep), &rep_sz, 1000);
srr_udp_close(cli);
```

### Tier 4: Embedded 32-bit Serial UART (`srr_serial.h` / `srr_embedded.h`)
```c
#include "srr/srr_serial.h"

srr_serial_port_t port;
srr_serial_config_t cfg;
srr_serial_default_config(&cfg);
srr_serial_open(&port, "/dev/ttyUSB0", &cfg);

// Synchronous device request (Header service_id=1, operation_id=2)
uint8_t reply[64];
size_t actual_len = 0;
int rc = srr_serial_request(&port, 1, 2, "STATUS", 6, reply, sizeof(reply), &actual_len, 250);
srr_serial_close(&port);
```

### Tier 5: Deep Embedded 8-bit MCU (AVR ATmega328P - `srr_tiny.h`)
*Strictly zero heap. Total RAM usage 335 bytes.*
```c
#include "srr/srr_tiny.h"

static srr_tiny_endpoint_t ep;

static uint8_t handle_cmd(uint16_t op, const uint8_t *req, uint8_t req_len, uint8_t *rep, uint8_t rep_cap, void *ud) {
    rep[0] = 0xAA; // Status OK
    return 1;      // Reply length
}

void setup(void) {
    srr_tiny_init(&ep, 0x1234);
    srr_tiny_register_service(&ep, 0x0001, handle_cmd, NULL);
}

void loop(void) {
    while (Serial.available()) {
        uint8_t c = Serial.read();
        uint8_t tx_frame[66];
        size_t tx_len = srr_tiny_feed_byte(&ep, c, tx_frame, sizeof(tx_frame));
        if (tx_len > 0) {
            Serial.write(tx_frame, tx_len); // Write reply frame
        }
    }
}
```

### Tier 6: Multi-Core Propeller 1 Hub Mailbox (`srr_p1_hub.h`)
```c
#include "srr/srr_p1_hub.h"

// Hub memory shared across all 8 Cogs
static srr_p1_hub_t hub_state;

void main(void) {
    srr_p1_hub_init(&hub_state, 0x1234, 0); // Lock 0
    // Worker Cog Loop:
    uint16_t svc = 0, op = 0;
    uint8_t payload[64];
    uint8_t len = 0;

    if (srr_p1_worker_claim(&hub_state, &svc, &op, payload, &len)) {
        uint8_t rep[4] = {0, 1, 2, 3};
        srr_p1_worker_reply(&hub_state, rep, 4);
    }
}
```

### Tier 7: WebAssembly / Web Workers (`SharedArrayBuffer` + `Atomics`)
*See [docs/WASM_WEB_WORKERS_SRR.md](docs/WASM_WEB_WORKERS_SRR.md) for full architecture.*
```javascript
// Worker A (Client) talking to Worker B (Server) in Wasm:
// Synchronous srr_send() blocks cleanly via Atomics.wait() inside Web Workers!

// Main UI Thread (Non-blocking Promise wrapper):
const reply = await srrClient.send("audio_synth", { cmd: "SET_FREQ", freq: 440 });
```

---

## 6. Idiomatic Payload Design: Structs, Unions & Zero-Copy Dispatch

Agents must never use dynamic text parsers (JSON, XML) on embedded or real-time tiers. Use the **QNX-Style Struct & Union Pattern**:

```c
// 1. Schema Definitions (messages.h)
typedef enum {
    MSG_SET_SPEED = 0x0101,
    MSG_GET_TELEM = 0x0102
} msg_type_t;

typedef struct { uint16_t type; uint16_t motor_id; float rpm; } req_set_speed_t;
typedef struct { uint16_t type; uint16_t motor_id; }            req_get_telem_t;

typedef union {
    uint16_t        type;
    req_set_speed_t set_speed;
    req_get_telem_t get_telem;
} server_msg_union_t;

// 2. Server Zero-Copy Dispatch Loop
void run_server(srr_channel_t *channel) {
    srr_request_t *req = NULL;
    while (srr_receive(channel, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            srr_request_dispose(req);
            continue;
        }
        const server_msg_union_t *msg = (const server_msg_union_t *)srr_request_data(req);
        size_t sz = srr_request_size(req);

        switch (msg->type) {
        case MSG_SET_SPEED:
            if (sz >= sizeof(req_set_speed_t)) {
                apply_speed(msg->set_speed.motor_id, msg->set_speed.rpm);
                int32_t ack = 0;
                srr_reply(req, &ack, sizeof(ack));
            }
            break;
        case MSG_GET_TELEM:
            // Reply with stack struct
            srr_reply(req, &cached_telem, sizeof(cached_telem));
            break;
        default:
            srr_reply_error(req, SRR_INVALID);
            break;
        }
    }
}
```

### Zero-Copy Scatter-Gather I/O (`srr_sendv` / `srr_replyv`)
On Tier 1 (In-Process) and Tier 2 (Host OS IPC), agents can eliminate intermediate buffer copies by transmitting disjoint memory regions (e.g. command header + bulk data) directly:

```c
// CLIENT: Transmit header struct + data buffer without packing into a single buffer
cmd_header_t hdr = { .cmd_id = 1 };
srr_iovec_t send_iov[2] = {
    { .iov_base = &hdr,    .iov_len = sizeof(hdr) },
    { .iov_base = raw_buf, .iov_len = raw_len }
};
srr_iovec_t rep_iov[2] = {
    { .iov_base = &rep_hdr, .iov_len = sizeof(rep_hdr) },
    { .iov_base = out_buf,  .iov_len = out_cap }
};
srr_status_t rc = srr_sendv(conn, send_iov, 2, rep_iov, 2, &out_reply_size);

// SERVER: Reply with non-contiguous status header + telemetry data
srr_iovec_t srv_rep[2] = {
    { .iov_base = &status, .iov_len = sizeof(status) },
    { .iov_base = &telem,  .iov_len = sizeof(telem) }
};
srr_replyv(req, srv_rep, 2);
```

---

## 7. Status Codes & Error Handling Reference

| Status Enum | Value | Meaning & Agent Remediation |
| :--- | :---: | :--- |
| `SRR_OK` | `0` | Success. Proceed normally. |
| `SRR_CHANNEL_CLOSED` | `1` | Server channel is closing. Exit worker loop cleanly. |
| `SRR_TIMEOUT` | `2` | Deadline expired before server replied. Transaction was cancelled; client is safe. |
| `SRR_CONNECTION_CLOSED` | `3` | Connection to server was terminated. Reconnect before retrying. |
| `SRR_SERVER_DIED` | `4` | Server crashed while holding request. Re-resolve/reconnect; do not reuse handle. |
| `SRR_QUEUE_FULL` | `5` | Server capacity reached. Back off and retry. |
| `SRR_TOO_LARGE` | `6` | Payload exceeds tier capacity. Check Section 3 table and reduce payload size. |
| `SRR_PROTOCOL_ERROR` | `7` | Corrupted wire header, bad magic, or CRC-16 mismatch. Check baud/framing. |
| `SRR_NO_MEMORY` | `8` | Allocation failure. Reduce queue depth or static buffers. |
| `SRR_INVALID` | `9` | NULL pointer, invalid parameter, or state machine violation. |

---

## 8. Verification & Test Targets Cheatsheet

Before committing any modifications to SRR, agents must execute these commands:
```bash
# 1. Standard build & test suite (13 unit suites)
make clean && make -j$(nproc) && make test

# 2. Strict Memory Safety (Valgrind: 0 errors, 0 memory leaks)
make test-valgrind

# 3. Sanitizers (ThreadSanitizer & AddressSanitizer)
make test-asan
make test-tsan

# 4. Cross-Language Conformance Suite
make test-conformance

# 5. Language Adapters (BEAM + Python + Gateway)
make test-adapters

# 6. Windows Cross-Compilation (MinGW)
make win32
```

---

## Appendix: The S-Suite Architectural Roadmap

To build higher-level services on top of `srr` without violating its core invariants, agents and engineers must align with the following modular repository architecture:

```
               +---------------------------------------+
               |                 sio                   |
               | (POSIX I/O shims: open, read, write)  |
               +-------------------+-------------------+
                                   |
         +-------------------------+-------------------------+
         |                         |                         |
         v                         v                         v
+-----------------+       +-----------------+       +-----------------+
|       srm       |       |      snet       |       |      stmr       |
|  (ResMgr / Path |       | (Qnet / Multi-  |       |  (Periodic      |
|    Namespace)   |       |  Node Routing)  |       | Pulse Engine)   |
+--------+--------+       +--------+--------+       +--------+--------+
         |                         |                         |
         +-------------------------+-------------------------+
                                   |
                                   v
               +---------------------------------------+
               |                  srr                  |
               | (The Core Engine: Send-Receive-Reply) |
               +---------------------------------------+
```

### 1. `srm` (SRR Resource Manager)
* **Goal:** Path mounting and server dispatch loop abstraction.
* **Invariant:** Every registered path (`/dev/motor`) maps to an `srr_channel_t`. The dispatch loop automatically handles capability negotiation and client disconnects.
* **Sketch:** `srm_attach(ctx, "/dev/motor", &dispatch_table);`

### 2. `sio` (SRR Input / Output)
* **Goal:** Standard POSIX file message structures (`_IO_OPEN`, `_IO_READ`, `_IO_WRITE`, `_IO_DEVCTL`).
* **Invariant:** Pure payload definitions. Never reimplements transport. Provides client-side shims that wrap `read()`/`write()` calls into `srr_send()`.

### 3. `snet` (SRR Network Router - The "Qnet" of SRR)
* **Goal:** Transparent multi-node proxying.
* **Invariant:** If a service path specifies a remote host (`/net/nodeB/dev/motor`), `snet` intercepts the connection, discovers the node via UDP broadcast/mDNS, and proxies the 32-byte wire frames across network sockets.

### 4. `stmr` (SRR Timer & Pulse Engine)
* **Goal:** High-resolution periodic pulse delivery.
* **Invariant:** Timers never invoke arbitrary user callbacks. They inject 64-bit `srr_pulse()` messages directly into target channels, keeping server event loops single-threaded and $O(1)$.

### 5. `swasm` (SRR WebAssembly & Browser Mesh)
* **Goal:** Web Worker mesh orchestrator and JavaScript Promise facade.
* **Invariant:** Web Workers communicate peer-to-peer using synchronous C SRR over `SharedArrayBuffer` (`Atomics.wait`). The Main UI thread uses non-blocking Promise facades (`await srr.send()`) to guarantee zero UI frame drops.
