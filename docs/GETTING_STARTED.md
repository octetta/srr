# Getting Started with SRR

This guide walks you through building concurrent, message-driven C applications with **SRR (Send / Receive / Reply)**.

---

## What SRR Is Good For & How to Use It

### Why Use SRR in Your System?
* **One Contract for Polyglot Software & Hardware:** The exact same Send / Receive / Reply code connects high-level host software (Go, Python, Node, Erlang) to C background services and microcontrollers (Pico, AVR, ESP32).
* **Message-Oriented, Never Stream-Oriented:** Delivers discrete, all-or-nothing message packets. You never write partial read loops, track byte counters, or parse delimiters.
* **Immune to the Stale-Reply Trap:** Unlike raw sockets where timed-out requests leave behind stale responses that corrupt future reads, SRR's single-winner state machine automatically discards late replies by transaction ID.
* **Message Passing Without Mutexes:** Like Go channels or Erlang mailboxes, SRR replaces complex mutex lock orchestration with clean, channel-based message passing (`6.7 µs` in-process).
* **Zero-Heap Microcontroller Profiles:** Runs without dynamic heap memory (`malloc`), fitting into as little as 335 bytes of RAM on an 8-bit Arduino.

### The Core Interaction Model

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant SRR as SRR Channel / Wire
    actor Server

    Note over Client,Server: Mode 1: Synchronous Request-Reply (Control / RPC)
    Client->>SRR: srr_send(conn, request_data)
    activate Client
    Note over Client: Client blocks safely
    SRR->>Server: srr_receive(chan, &req)
    activate Server
    Note over Server: Server processes request
    Server->>SRR: srr_reply(req, reply_data)
    deactivate Server
    SRR-->>Client: Reply delivered (unblocks client)
    deactivate Client

    Note over Client,Server: Mode 2: Asynchronous Pulse (Telemetry / Ticks / Alarms)
    Client-)SRR: srr_pulse(conn, code, value)
    Note over Client: Fire-and-forget (non-blocking)
    SRR-)Server: Delivered to srr_receive queue
```

### The Adopter's Mental Model: 3 Core Shifts

1. **Think in Transactions, Not Byte Streams:**
   You never write loops to handle partial reads, track byte counts, search for newline `\n` delimiters, or assemble fragmented TCP/serial streams. You submit an entire message and receive an entire reply. If a timeout occurs, that transaction is permanently dead—it will never pollute future reads.

2. **The Two Interaction Primitives (Send vs. Pulse):**
   * **Need an answer or confirmation?** Use `srr_send()` (Client blocks safely until Server answers). Ideal for RPC commands, parameter updates, and queries.
   * **Fire-and-forget event or telemetry?** Use `srr_pulse()` (Client never blocks, Server never replies). Ideal for 100 Hz sensor ticks, heartbeats, alarms, and GPIO triggers.

3. **Code to Services, Not Physical Cables:**
   Avoid hardcoding `/dev/ttyUSB0` or IP addresses in business logic. Connect to a symbolic service name (`"motor_controller"`). Whether that service lives across a thread pointer, a Unix socket, a UDP network datagram, or an Arduino over USB serial, your application code is identical.

---

### The Golden Rule for Servers: "Always Answer or Dispose"

When a server dequeues a request from `srr_receive(chan, &req)`, the client is suspended waiting for a resolution. The server **must** do one of the following:

* **Success:** `srr_reply(req, reply_data, len)` — delivers response data and unblocks client with `SRR_OK`. Automatically disposes of the request handle.
* **Business Error:** `srr_reply_error(req, SRR_INVALID)` — unblocks client with an explicit error code. Automatically disposes of the request handle.
* **Cancellation / Abandonment:** `srr_request_dispose(req)` — drops the request and safely unblocks the client with `SRR_CANCELLED` (no hanging clients).
* **If it's a Pulse (`srr_request_is_pulse(req)`):** Read `srr_pulse_code()` and `srr_pulse_value()`, then call `srr_request_dispose(req)`. (Never call `srr_reply()` on a pulse).

---

## 1. Quick Tutorial: In-Process Thread Messaging

In SRR, threads communicate through **channels** and **connections** without sockets or network overhead.

### Complete Working Example (`example_threads.c`)

```c
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include "srr/srr.h"

static void *server_worker(void *arg) {
    srr_channel_t *channel = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    // Block until requests arrive
    while (srr_receive(channel, &req) == SRR_OK) {
        const char *input = (const char *)srr_request_data(req);

        char reply[128];
        snprintf(reply, sizeof(reply), "PONG: %s", input);

        // Reply to client and unblock client thread
        srr_reply(req, reply, strlen(reply) + 1);
    }
    return NULL;
}

int main(void) {
    // 1. Server creates in-process channel
    srr_channel_t *channel = NULL;
    assert(srr_channel_create(NULL, &channel) == SRR_OK);

    // 2. Start server receiver thread
    pthread_t server_th;
    pthread_create(&server_th, NULL, server_worker, channel);

    // 3. Client connects directly to in-process channel
    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(channel, NULL, &conn) == SRR_OK);

    // 4. Client issues synchronous send
    const char *msg = "ping!";
    char reply[128] = {0};
    size_t reply_sz = 0;

    srr_status_t status = srr_send(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz);
    if (status == SRR_OK) {
        printf("Client received: %s\n", reply);
    }

    // 5. Cleanup
    srr_disconnect(conn);
    srr_channel_close(channel);
    pthread_join(server_th, NULL);
    srr_channel_destroy(channel);
    return 0;
}
```

### Compiling and Running
```bash
gcc -Wall -Wextra -pthread -Iinclude example_threads.c -L. -lsrr -o example_threads
./example_threads
```

---

## 2. Moving Across Process Boundaries (Zero Code Changes!)

To make the exact same service communicate across distinct OS processes:

### Server Process
Change channel creation to specify a service name:
```c
srr_channel_create_service("echo_service", NULL, &channel);
```

### Client Process
Change client connection to specify the service name:
```c
srr_connect("echo_service", NULL, &conn);
```

**That is the only change.**
The request processing, message payload handling, timeout semantics, and reply logic remain 100% identical.

---

## 3. Handling Timeouts & Deadlines

Use `srr_send_timed()` to enforce maximum wait times:

```c
// Timeout after 250 milliseconds
srr_status_t status = srr_send_timed(
    conn,
    request_data, request_len,
    reply_buf, sizeof(reply_buf),
    &reply_size,
    250
);

if (status == SRR_TIMEOUT) {
    printf("Server did not reply within 250ms\n");
}
```

---

## 4. One-Way Pulses (Heartbeats / Event Ticks)

For lightweight, non-blocking notifications that require no reply:

### Client
```c
// Send a pulse with 32-bit code (e.g. 1 = CLOCK_TICK) and 64-bit value
srr_pulse(conn, 1, 100);
```

### Server
```c
srr_request_t *item = NULL;
if (srr_receive(channel, &item) == SRR_OK) {
    if (srr_request_is_pulse(item)) {
        uint32_t code = srr_pulse_code(item);
        uint64_t val = srr_pulse_value(item);
        printf("Received pulse: code=%u, value=%lu\n", code, (unsigned long)val);

        // Always dispose pulses (do not call srr_reply!)
        srr_request_dispose(item);
        continue;
    }
}
```

---

## 5. Using the Language Adapters

### Erlang/OTP
```erlang
{ok, Client} = srr_client:start_link("echo_service").
{ok, Reply} = srr_client:call(Client, <<"hello">>).
ok = srr_client:pulse(Client, 1, 42).
srr_client:stop(Client).
```

### Elixir
```elixir
{:ok, client} = SRR.start_link("echo_service")
{:ok, reply} = SRR.call(client, "hello")
:ok = SRR.pulse(client, 1, 42)
SRR.stop(client)
```

### Web Browser (WebSocket Gateway)
Run the gateway:
```bash
python3 gateway/web_gateway.py
```
Send JSON commands from JavaScript:
```javascript
const ws = new WebSocket("ws://127.0.0.1:8081");
ws.onopen = () => {
  ws.send(JSON.stringify({ action: "send", cmd: "STATUS" }));
};
ws.onmessage = (event) => {
  const res = JSON.parse(event.data);
  console.log("SRR reply:", res.reply);
};
```
