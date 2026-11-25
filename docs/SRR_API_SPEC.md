# SRR Public API Specification

Version: 1.0  
Language: C99 / Portable C  
Compliance: POSIX Threads & Bare-Metal Embedded  

---

## 1. Primary Objects

```c
typedef struct srr_channel    srr_channel_t;
typedef struct srr_connection srr_connection_t;
typedef struct srr_request    srr_request_t;
```

---

## 2. Status & Error Representation

```c
typedef enum {
    SRR_OK = 0,
    SRR_TIMEOUT,            /* Operation timed out */
    SRR_CANCELLED,          /* Request cancelled/abandoned */
    SRR_DISCONNECTED,       /* Connection broken */
    SRR_SERVER_DIED,        /* Server crashed or closed without reply */
    SRR_CLOSED,             /* Channel was closed */
    SRR_QUEUE_FULL,         /* Queue depth reached (backpressure) */
    SRR_TOO_LARGE,          /* Payload exceeds buffer bounds */
    SRR_PROTOCOL_ERROR,     /* Framing / protocol violation */
    SRR_NO_MEMORY,          /* Dynamic allocation failed */
    SRR_INVALID             /* Invalid parameter or state */
} srr_status_t;
```

---

## 3. Server-Side Channel API

### `srr_channel_create`
```c
srr_status_t srr_channel_create(
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);
```
- **Blocking:** No.
- **Thread Safety:** N/A (creates new instance).
- **Ownership:** Caller owns the created `*out_channel` and must eventually destroy it.

### `srr_channel_close`
```c
srr_status_t srr_channel_close(srr_channel_t *channel);
```
- **Blocking:** No (wakes blocked receivers immediately with `SRR_CLOSED`).
- **Thread Safety:** Safe to call concurrently with `srr_receive()`.

### `srr_channel_destroy`
```c
void srr_channel_destroy(srr_channel_t *channel);
```
- **Blocking:** Waits for reference counts to reach zero if threads are actively draining.
- **Thread Safety:** Must only be called once per channel.

### `srr_receive`
```c
srr_status_t srr_receive(
    srr_channel_t *channel,
    srr_request_t **out_request);
```
- **Blocking:** Yes (sleeps until a request/pulse is queued or channel closes).
- **Thread Safety:** Safe for multiple receiver threads simultaneously.
- **Ownership:** Transfers ownership of `*out_request` to the calling receiver thread.

---

## 4. Server-Side Request Handling

### `srr_reply`
```c
srr_status_t srr_reply(
    srr_request_t *request,
    const void *reply_data,
    size_t reply_size);
```
- **Single-Winner:** Only the first call to `reply` or `reply_error` succeeds.
- **Ownership:** Automatically frees/disposes `request`. Do not call `srr_request_dispose()` after `srr_reply()`.

### `srr_reply_error`
```c
srr_status_t srr_reply_error(
    srr_request_t *request,
    srr_status_t error);
```
- Transmits an explicit failure status to the waiting client.

### `srr_request_dispose`
```c
void srr_request_dispose(srr_request_t *request);
```
- Disposes of an unreplied request (client receives `SRR_CANCELLED`) or frees a received pulse.

---

## 5. Client-Side Connection API

### `srr_connect_channel` / `srr_connect`
```c
srr_status_t srr_connect_channel(
    srr_channel_t *channel,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);

srr_status_t srr_connect(
    const char *service_name,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);
```
- Establishes a client connection handle.

### `srr_send` / `srr_send_timed`
```c
srr_status_t srr_send(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size);

srr_status_t srr_send_timed(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size,
    uint32_t timeout_ms);
```
- **Blocking:** Blocks until server replies or `timeout_ms` expires.
- **Ownership:** `request_data` is copied into transaction storage before the call blocks. `reply_buffer` is populated before return.

### `srr_pulse`
```c
srr_status_t srr_pulse(
    srr_connection_t *connection,
    uint32_t code,
    uint64_t value);
```
- **Blocking:** No.
- **Return:** `SRR_OK` or `SRR_QUEUE_FULL`.

---

## 6. Cooperative / Embedded Profile API

For bare-metal microcontrollers with no RTOS, threads, or heap allocation:

```c
typedef struct srr_wire_parser srr_wire_parser_t;

/* Initialize parser with caller-provided static buffer */
void srr_wire_parser_init(
    srr_wire_parser_t *parser,
    uint8_t *buffer,
    size_t capacity);

/* Feed incoming byte (from UART ISR or buffer) */
bool srr_wire_parser_feed_byte(
    srr_wire_parser_t *parser,
    uint8_t byte,
    srr_frame_t *out_frame);

/* Feed incoming chunk */
size_t srr_wire_parser_feed_chunk(
    srr_wire_parser_t *parser,
    const uint8_t *data,
    size_t len,
    srr_frame_t *out_frame);
```
