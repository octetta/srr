# SRR: Synchronous Message Passing for Portable Concurrent Systems

> **SRR brings a QNX-inspired Send / Receive / Reply service model to ordinary C programs, natively across threads and across local processes without changing the service programming contract.**

---

## Highlights

- **Blocking Transaction Semantics:** A client calls `srr_send()` and blocks until the server executes `srr_reply()` or a terminal condition occurs (timeout, disconnect, server died).
- **Single-Winner Resolution:** Concurrent reply, timeout, disconnect, and teardown paths are strictly synchronized—a transaction never completes twice.
- **Unified Programming Model:** The identical C API operates between in-process threads (`pthreads`) and across processes (POSIX Unix Domain Sockets & Windows Named Pipes).
- **Microsecond Latency:** ~6.6 µs median in-process round-trip latency; up to **447,000 transactions/sec** on multi-core systems.
- **Zero Leaks:** 100% verified under Valgrind and ThreadSanitizer with zero memory leaks and zero race conditions.
- **Language Adapters:** Native BEAM (Erlang `gen_server` and Elixir) integration and WebSocket Browser gateway.

---

## Architecture Overview

```text
                     Public C API (include/srr/srr.h)
                                    |
                    Core Transaction State Machine
                (Single-winner, bounded queue, lifetimes)
                                    |
                  +-----------------+-----------------+
                  |                                   |
           pthread Backend                       IPC Backend
        (In-Process Queues)            (Framed Streaming Protocol)
                  |                                   |
         Pthread Mutex / CV              +------------+------------+
                                         |                         |
                                       POSIX                    Windows
                                (Unix Domain Sockets)        (Named Pipes)
```

### Transaction State Machine

```text
CREATED
   |
   v
QUEUED
   |
   v
DELIVERED
   |
   +------> REPLIED ------> COMPLETED
   |
   +------> CANCELLED
   |
   +------> TIMED_OUT
   |
   +------> DISCONNECTED
   |
   +------> SERVER_DIED
   |
   +------> ERROR
```

---

## Quickstart

### Server
```c
#include <stdio.h>
#include <string.h>
#include "srr/srr.h"

int main(void) {
    srr_channel_t *channel = NULL;
    // Binds in-process channel (or cross-process if service_name is set)
    srr_channel_create_service("echo_svc", NULL, &channel);

    srr_request_t *req = NULL;
    while (srr_receive(channel, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            printf("Pulse code=%u value=%lu\n", srr_pulse_code(req), (unsigned long)srr_pulse_value(req));
            srr_request_dispose(req);
            continue;
        }

        const char *data = (const char *)srr_request_data(req);
        char reply[128];
        snprintf(reply, sizeof(reply), "ECHO: %s", data);

        // Synchronously replies to client and unblocks client's srr_send()
        srr_reply(req, reply, strlen(reply) + 1);
    }

    srr_channel_destroy(channel);
    return 0;
}
```

### Client
```c
#include <stdio.h>
#include <string.h>
#include "srr/srr.h"

int main(void) {
    srr_connection_t *conn = NULL;
    srr_connect("echo_svc", NULL, &conn);

    const char *msg = "hello world";
    char reply[128] = {0};
    size_t reply_sz = 0;

    // Blocks until server replies or 1000ms timeout expires
    srr_status_t status = srr_send_timed(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz, 1000);

    if (status == SRR_OK) {
        printf("Server replied: %s\n", reply);
    } else {
        printf("Failed: %s\n", srr_status_str(status));
    }

    // Send an asynchronous one-way notification pulse
    srr_pulse(conn, 1, 42);

    srr_disconnect(conn);
    return 0;
}
```

---

## Benchmarks (-O3, 64-byte payload)

| Scenario | Throughput | p50 (Median) | p95 | p99 |
|---|---|---|---|---|
| **In-Process `pthread`** *(1 Client $\rightarrow$ 1 Server)* | **144,364 ops/sec** | **6.63 µs** | 8.71 µs | 14.76 µs |
| **In-Process Multi-Core** *(4 Clients $\rightarrow$ 4 Servers)* | **447,262 ops/sec** | **7.62 µs** | 15.53 µs | 22.31 µs |
| **Process IPC / UDS** *(1 Client $\rightarrow$ 1 Server)* | **55,055 ops/sec** | **16.75 µs** | 24.87 µs | 32.48 µs |

---

## Adapters & Integrations

- **BEAM (Erlang / OTP):** [adapters/beam/src/srr_client.erl](adapters/beam/src/srr_client.erl) provides a native `gen_server` mapping `call/3` directly to SRR transactions over Unix Domain Sockets.
- **BEAM Port Driver:** [adapters/beam/src/srr_port_driver.c](adapters/beam/src/srr_port_driver.c) provides a standard `{packet, 4}` port process for Erlang/Elixir supervisors.
- **Elixir Package:** [adapters/elixir/](adapters/elixir/) provides idiomatic Hex package structure (`SRR.start_link/2`, `SRR.call/3`, `SRR.pulse/3`).
- **Browser WebAudio Gateway:** [gateway/](gateway/) provides a zero-dependency WebSocket gateway and interactive WebAudio dashboard with keyboard controls.

---

## Building and Testing

```bash
# Build static library libsrr.a, CLI examples, and BEAM port driver
make all

# Run complete unit test suite (basic, timeout/races, multi-client stress, cross-process fork)
make test

# Run Valgrind memory leak verification
make test-valgrind

# Run performance microbenchmarks (-O3)
make bench

# Cross-compile for Windows (MinGW Named Pipes)
make win32
```
