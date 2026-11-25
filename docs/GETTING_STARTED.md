# Getting Started with SRR

This guide walks you through building concurrent, message-driven C applications with **SRR (Send / Receive / Reply)**.

---

## What SRR Is Good For & How to Use It

### What SRR Is Good For
* **Uniform Multi-Tier Architecture:** One communication contract spans in-process threads, local OS processes, network UDP sockets, and bare-metal microcontrollers (Pico, AVR, ESP32, Propeller P1).
* **Deterministic Single-Winner RPC:** Transactions never complete twice. Concurrent timeouts, late server replies, and client disconnects are arbitrated safely without stale replies or lost wakeups.
* **Zero-Heap Microcontroller Profiles:** Runs without dynamic heap memory (`malloc`), preventing fragmentation on 8-bit and 32-bit embedded targets.
* **Polyglot Interoperability:** C services can be driven natively from Go, Python, Perl, Node.js, Ruby, PHP, Erlang, Elixir, Gleam, Tcl, and Shell.

### The Core Interaction Model
```text
       Synchronous Transaction (Control / RPC)
Client ────────────────── srr_send() ──────────────────► Server (blocks client)
Client ◄───────────────── srr_reply() ───────────────── Server (unblocks client)

       Asynchronous Notification (Telemetry / Ticks / Alarms)
Client ────────────────── srr_pulse() ─────────────────► Server (non-blocking)
```

1. **Server**: Calls `srr_channel_create()`, waits in a loop with `srr_receive()`, processes the request, and unblocks the client with `srr_reply()`.
2. **Client**: Calls `srr_connect()`, issues a synchronous request with `srr_send()` or `srr_send_timed()`, and blocks safely until the server responds.
3. **Pulses**: For one-way notifications (sensor telemetry, periodic ticks, emergency stops), either endpoint calls `srr_pulse()` without blocking or waiting for an answer.

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
