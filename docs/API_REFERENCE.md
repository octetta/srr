# SRR C API Reference

This document provides the formal reference for the public SRR C API declared in [`include/srr/srr.h`](../include/srr/srr.h).

---

## 1. Types and Status Codes

### Opaque Handles

```c
typedef struct srr_channel    srr_channel_t;
typedef struct srr_connection srr_connection_t;
typedef struct srr_request    srr_request_t;
```

- **`srr_channel_t`:** A server-owned endpoint that receives incoming requests and pulses into a bounded queue. Multiple server threads may call `srr_receive()` concurrently on the same channel.
- **`srr_connection_t`:** A client-side handle bound to a local channel or IPC service. Thread-safe: serializes concurrent sends per connection.
- **`srr_request_t`:** A handle representing an incoming request or pulse delivered to a server thread. Must be answered via `srr_reply()`, `srr_reply_error()`, or released via `srr_request_dispose()`.

---

### Status Codes (`srr_status_t`)

| Code | Value | Description |
|---|:---:|---|
| `SRR_OK` | 0 | Operation completed successfully. |
| `SRR_TIMEOUT` | 1 | Request timed out before a reply arrived. |
| `SRR_CANCELLED` | 2 | Request was abandoned or cancelled. |
| `SRR_DISCONNECTED` | 3 | Connection to remote service or channel was severed. |
| `SRR_SERVER_DIED` | 4 | Server closed or crashed before replying. |
| `SRR_CLOSED` | 5 | Channel was closed to new operations. |
| `SRR_QUEUE_FULL` | 6 | Channel request queue has reached capacity (backpressure). |
| `SRR_TOO_LARGE` | 7 | Message or reply payload exceeded buffer capacity. |
| `SRR_PROTOCOL_ERROR` | 8 | Framing or state machine violation detected on transport. |
| `SRR_NO_MEMORY` | 9 | Out of memory during buffer allocation. |
| `SRR_INVALID` | 10 | Invalid argument or parameter. |

```c
const char *srr_status_str(srr_status_t status);
```
Returns a static string representation of the given status code (e.g. `"SRR_TIMEOUT"`).

---

## 2. Server API: Channel Lifecycle

### `srr_channel_create`
```c
srr_status_t srr_channel_create(
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);
```
Creates a new server channel endpoint.
- **`options`:** Pointer to `srr_channel_options_t`, or `NULL` for defaults (`queue_capacity = 64`, `max_request_size = 64KB`, `max_reply_size = 64KB`).
- If `options->service_name` is non-NULL, automatically binds a cross-process IPC service (Unix Domain Socket on POSIX, Named Pipe on Windows).
- **Returns:** `SRR_OK` on success, `SRR_NO_MEMORY` or `SRR_INVALID` on failure.

### `srr_channel_create_service`
```c
srr_status_t srr_channel_create_service(
    const char *service_name,
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);
```
Convenience function to bind an IPC service channel by name.

### `srr_channel_close`
```c
srr_status_t srr_channel_close(srr_channel_t *channel);
```
Closes the channel to incoming sends:
- Rejects new client sends with `SRR_CLOSED`.
- Wakes all threads blocked in `srr_receive()` (they unblock with `SRR_CLOSED`).
- Drains unreceived queued requests, completing them with `SRR_SERVER_DIED`.
- Active, already-delivered requests remain valid until replied to or disposed.

### `srr_channel_destroy`
```c
void srr_channel_destroy(srr_channel_t *channel);
```
Closes the channel if not already closed and releases internal resources once active reference counts reach zero. Safe to call while receiver threads are exiting.

### `srr_receive`
```c
srr_status_t srr_receive(
    srr_channel_t *channel,
    srr_request_t **out_request);
```
Blocks until a request or pulse is available on `channel`, or until the channel is closed.
- Multiple receiver threads can wait on the same channel simultaneously.
- On success, sets `*out_request`. The caller **must** complete the request via `srr_reply()`, `srr_reply_error()`, or `srr_request_dispose()`.

---

## 3. Server API: Handling Requests & Pulses

### Pulse Introspection
```c
bool     srr_request_is_pulse(const srr_request_t *request);
uint32_t srr_pulse_code(const srr_request_t *request);
uint64_t srr_pulse_value(const srr_request_t *request);
```
- `srr_request_is_pulse`: Returns `true` if the item is a one-way notification pulse.
- `srr_pulse_code` / `srr_pulse_value`: Returns the 32-bit code and 64-bit value sent with the pulse.
- For pulses, call `srr_request_dispose(request)` after processing. Do **not** call `srr_reply()`.

### Request Data Accessors
```c
const void *srr_request_data(const srr_request_t *request);
size_t      srr_request_size(const srr_request_t *request);
```
Returns a read-only pointer to transaction-owned request payload bytes and the payload size. Remains valid until replied or disposed.

### `srr_reply`
```c
srr_status_t srr_reply(
    srr_request_t *request,
    const void *reply_data,
    size_t reply_size);
```
Replies to the client and unblocks the client's `srr_send()`.
- **Single-Winner:** Only the first call to `srr_reply()` or `srr_reply_error()` completes the transaction.
- If the client already timed out or disconnected, returns `SRR_TIMEOUT` or `SRR_DISCONNECTED` to inform the server.
- Automatically disposes of the request handle—no separate `srr_request_dispose()` call is needed.

### `srr_reply_error`
```c
srr_status_t srr_reply_error(
    srr_request_t *request,
    srr_status_t error);
```
Replies to the client with an explicit error code. The client's `srr_send()` will return `error`. Automatically disposes of the request handle.

### `srr_request_dispose`
```c
void srr_request_dispose(srr_request_t *request);
```
Explicitly releases a request handle without replying. If the client is still waiting, the client's `srr_send()` unblocks with `SRR_CANCELLED`. Safe to call on already-replied requests (no-op).

---

## 4. Client API: Connections & Messaging

### `srr_connect_channel`
```c
srr_status_t srr_connect_channel(
    srr_channel_t *channel,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);
```
Connects a client directly to an in-process `srr_channel_t` pointer. Bypasses all socket/pipe layers for microsecond latency.

### `srr_connect`
```c
srr_status_t srr_connect(
    const char *service_name,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);
```
Connects to an IPC service by name across process boundaries (`/tmp/srr_<name>.sock` on POSIX, `\\\\.\\pipe\\srr_<name>` on Windows).

### `srr_disconnect`
```c
void srr_disconnect(srr_connection_t *connection);
```
Disconnects the client handle and frees client-side state. Any blocked `srr_send()` calls on this connection will wake with `SRR_DISCONNECTED`.

### `srr_send`
```c
srr_status_t srr_send(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size);
```
Synchronously transmits a request and blocks until a reply is received:
- Request data is copied into library storage before the call blocks.
- Reply data is copied into `reply_buffer` upon return.
- If the reply payload exceeds `reply_capacity`, copies what fits, returns `SRR_TOO_LARGE`, and writes the required size to `*out_reply_size`.

### `srr_send_timed`
```c
srr_status_t srr_send_timed(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size,
    uint32_t timeout_ms);
```
Same as `srr_send()`, but bounds the maximum blocking time to `timeout_ms` milliseconds. If the timeout expires before a reply arrives, returns `SRR_TIMEOUT`.

### `srr_pulse`
```c
srr_status_t srr_pulse(
    srr_connection_t *connection,
    uint32_t code,
    uint64_t value);
```
Sends an asynchronous, non-blocking one-way pulse:
- Does not wait for a reply.
- Returns immediately with `SRR_OK`.
- Returns `SRR_QUEUE_FULL` if the channel queue depth is exhausted.

---

## 5. Wire Framing & Protocol Engine (`include/srr/srr_wire.h`)

For cross-boundary, UART, USB CDC, and microcontroller communication:

### CRC-16-CCITT
```c
uint16_t srr_crc16(const uint8_t *data, size_t len, uint16_t init_crc);
```
Calculates CCITT False checksum (poly `0x1021`, init `0xFFFF`) to detect line noise.

### COBS Framing
```c
size_t srr_cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);
size_t srr_cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);
```
Encodes and decodes arbitrary binary buffers with zero-byte delimiter `0x00` framing.

### Complete Frame Encoding
```c
size_t srr_wire_encode_frame(
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len,
    uint8_t *out_frame,
    size_t out_cap);
```
Packs the 32-byte Little-Endian binary header, application payload, and 2-byte CRC-16 into a COBS-encoded frame terminated with `0x00`.

### Streaming Chunk Parser (Zero Heap Allocation)
```c
void srr_wire_parser_init(srr_wire_parser_t *p, uint8_t *rx_buf, size_t rx_cap, uint8_t *decode_buf, size_t decode_cap);
void srr_wire_parser_reset(srr_wire_parser_t *p);
srr_parser_status_t srr_wire_parser_feed_byte(
    srr_wire_parser_t *p,
    uint8_t byte,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload);
```
Tolerates arbitrary byte chunking (1-byte feeds, half-frames, multiple back-to-back frames) with zero dynamic allocation.

---

## 6. Serial Transport API (`include/srr/srr_serial.h`)

Provides full-duplex communication over physical UART devices (`/dev/tty*`) and virtual links (`openpty`, `socketpair`):

```c
srr_serial_port_t* srr_serial_open(const char *device_path, const srr_serial_config_t *config);
srr_serial_port_t* srr_serial_from_fd(int fd, bool owns_fd, const srr_serial_config_t *config);
void srr_serial_close(srr_serial_port_t *port);

int srr_serial_request(
    srr_serial_port_t *port,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms);

int srr_serial_pulse(
    srr_serial_port_t *port,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len);
```

---

## 7. Embedded Cooperative Profile API (`include/srr/srr_embedded.h`)

Designed for bare-metal microcontrollers (Raspberry Pi Pico RP2040, ESP32, AVR Uno, Parallax P1) with zero dynamic allocation:

### Endpoint Lifecycle & Registration
```c
int srr_endpoint_init(
    srr_endpoint_t *ep,
    uint32_t session_id,
    srr_tx_fn_t tx_fn,
    void *tx_user_data,
    uint8_t *rx_buf, size_t rx_cap,
    uint8_t *decode_buf, size_t decode_cap,
    uint8_t *tx_buf, size_t tx_cap,
    uint8_t *server_reply_buf, size_t server_reply_cap);

int srr_endpoint_register_service(
    srr_endpoint_t *ep,
    uint16_t service_id,
    srr_service_handler_t handler,
    void *user_data);

int srr_endpoint_register_pulse_handler(
    srr_endpoint_t *ep,
    srr_pulse_handler_t handler,
    void *user_data);
```

### Main-Loop Byte Feeding & Timers
```c
srr_parser_status_t srr_endpoint_feed_byte(srr_endpoint_t *ep, uint8_t byte);
void                srr_endpoint_feed_bytes(srr_endpoint_t *ep, const uint8_t *data, size_t len);
void                srr_endpoint_tick(srr_endpoint_t *ep, uint32_t now_ms);
int                 srr_endpoint_send_pulse(srr_endpoint_t *ep, uint16_t service_id, uint16_t operation_id, const void *payload, size_t payload_len);
```

### Asynchronous Client Operations
```c
uint64_t          srr_begin_request(srr_endpoint_t *ep, uint16_t service_id, uint16_t operation_id, const void *req, size_t req_len, uint8_t *reply, size_t cap, uint32_t timeout_ms, uint32_t now_ms);
srr_trans_state_t srr_poll_transaction(srr_endpoint_t *ep, uint64_t trans_id);
int               srr_collect_reply(srr_endpoint_t *ep, uint64_t trans_id, size_t *out_reply_len, int *out_error_code);
int               srr_cancel_transaction(srr_endpoint_t *ep, uint64_t trans_id);
```

---

## 8. UDP Datagram Transport API (`include/srr/srr_udp.h`)

Enables synchronous Send/Receive/Reply and one-way pulses over local Ethernet / Wi-Fi UDP datagrams without TCP/TLS overhead.

### Configuration & Lifecycle
```c
void srr_udp_default_config(srr_udp_config_t *cfg);
srr_udp_endpoint_t* srr_udp_client_create(const char *remote_host, uint16_t remote_port, const srr_udp_config_t *config);
srr_udp_endpoint_t* srr_udp_server_create(uint16_t local_port, const srr_udp_config_t *config);
void srr_udp_close(srr_udp_endpoint_t *ep);
```

### Client Operations
```c
int srr_udp_request(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms
);

int srr_udp_pulse(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len
);
```

### Server Operations
```c
int srr_udp_server_receive(
    srr_udp_endpoint_t *ep,
    srr_wire_header_t *out_hdr,
    uint8_t *payload_buf,
    size_t payload_cap,
    size_t *actual_payload_len,
    srr_udp_peer_t **out_peer,
    uint32_t timeout_ms
);

int srr_udp_server_reply(
    srr_udp_endpoint_t *ep,
    const srr_udp_peer_t *peer,
    uint64_t trans_id,
    uint16_t service_id,
    uint16_t operation_id,
    const void *reply_payload,
    size_t reply_len,
    bool is_error
);

void srr_udp_peer_free(srr_udp_peer_t *peer);
```

---

## 9. Service & Device Resolver API (`include/srr/srr_resolver.h`)

Provides dynamic service registration, on-behalf-of embedded serial device proxies, and client endpoint resolution across IPC, Serial, and UDP transports.

### Resolver Engine Lifecycle
```c
srr_resolver_t* srr_resolver_create(void);
void            srr_resolver_destroy(srr_resolver_t *res);
int             srr_resolver_register(srr_resolver_t *res, const srr_endpoint_entry_t *entry);
int             srr_resolver_unregister(srr_resolver_t *res, const char *service_name);
int             srr_resolver_lookup(srr_resolver_t *res, const char *service_name, srr_endpoint_entry_t *out_entry);
int             srr_resolver_heartbeat(srr_resolver_t *res, const char *service_name);
int             srr_resolver_list(srr_resolver_t *res, srr_endpoint_entry_t *out_entries, size_t max_entries, size_t *out_count);
void            srr_resolver_prune_expired(srr_resolver_t *res, uint64_t now_ms);
int             srr_resolver_handle_request(srr_resolver_t *res, const char *req, char *rep, size_t rep_cap);
```

### Client Resolution & Registration
```c
srr_status_t srr_resolve(
    const char *resolver_service,
    const char *service_name,
    srr_endpoint_entry_t *out_entry,
    uint32_t timeout_ms
);

srr_status_t srr_register_service(
    const char *resolver_service,
    const srr_endpoint_entry_t *entry,
    uint32_t timeout_ms
);

srr_status_t srr_unregister_service(
    const char *resolver_service,
    const char *service_name,
    uint32_t timeout_ms
);
```

