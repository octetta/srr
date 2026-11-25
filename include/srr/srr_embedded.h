#ifndef SRR_EMBEDDED_H
#define SRR_EMBEDDED_H

#include "srr_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR Embedded Cooperative Endpoint (Zero Dynamic Memory Allocation)
 * ============================================================================
 * Designed for microcontrollers (RP2040 Pico, ESP32, AVR Uno, P1, bare metal).
 * Operates entirely cooperatively without threads, mutexes, or OS dependencies.
 */

#define SRR_EMBEDDED_MAX_SERVICES     8
#define SRR_EMBEDDED_MAX_TRANSACTIONS 4

/* Service Request Handler callback:
 * Returns 0 on success (populating reply_buf & out_reply_len),
 * or non-zero error code (which triggers SRR_WIRE_ERROR reply).
 */
typedef int (*srr_service_handler_t)(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *req_payload,
    size_t req_len,
    uint8_t *reply_buf,
    size_t reply_cap,
    size_t *out_reply_len,
    void *user_data
);

/* Asynchronous Pulse Handler callback */
typedef void (*srr_pulse_handler_t)(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *payload,
    size_t len,
    void *user_data
);

/* Byte Transmission Callback (e.g. uart_write_blocking) */
typedef int (*srr_tx_fn_t)(const uint8_t *data, size_t len, void *user_data);

/*
 * Concurrency & Thread-Safety:
 * srr_endpoint_t is designed for single-threaded / cooperative bare-metal loops.
 * If operations (e.g., srr_endpoint_feed_byte from a UART ISR and srr_begin_request
 * from application code or multiple FreeRTOS tasks) can execute concurrently, the caller
 * MUST provide external synchronization (e.g., mutex locking or critical section entry).
 */

/* Transaction State Machine */
typedef enum {
    SRR_TRANS_UNUSED = 0,
    SRR_TRANS_PENDING,
    SRR_TRANS_REPLIED,
    SRR_TRANS_TIMED_OUT,
    SRR_TRANS_CANCELLED,
    SRR_TRANS_ERROR
} srr_trans_state_t;

typedef struct {
    uint64_t trans_id;
    uint32_t session_id;
    srr_trans_state_t state;
    uint32_t timeout_at_ms;
    uint16_t service_id;
    uint16_t operation_id;
    uint8_t *reply_buf;
    size_t   reply_cap;
    size_t   reply_len;
    int      error_code;
} srr_async_trans_t;

typedef struct {
    uint16_t service_id;
    srr_service_handler_t handler;
    void *user_data;
} srr_service_entry_t;

typedef struct {
    uint32_t session_id;
    uint64_t next_trans_id;

    /* Outgoing physical transmission */
    srr_tx_fn_t tx_fn;
    void *tx_user_data;

    /* Pulse notification callback */
    srr_pulse_handler_t pulse_handler;
    void *pulse_user_data;

    /* Caller-provided zero-heap frame parser */
    srr_wire_parser_t parser;

    /* Caller-provided TX frame scratch buffer */
    uint8_t *tx_buf;
    size_t   tx_cap;

    /* Caller-provided Server reply scratch buffer */
    uint8_t *server_reply_buf;
    size_t   server_reply_cap;

    /* Registered services */
    srr_service_entry_t services[SRR_EMBEDDED_MAX_SERVICES];
    size_t service_count;

    /* Active async client transactions */
    srr_async_trans_t transactions[SRR_EMBEDDED_MAX_TRANSACTIONS];

    /* Statistics */
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_errors;
} srr_endpoint_t;

/*
 * Initialize embedded cooperative endpoint.
 * All memory buffers are provided by the caller (statically or on stack).
 */
int srr_endpoint_init(
    srr_endpoint_t *ep,
    uint32_t session_id,
    srr_tx_fn_t tx_fn,
    void *tx_user_data,
    uint8_t *rx_buf,
    size_t rx_cap,
    uint8_t *decode_buf,
    size_t decode_cap,
    uint8_t *tx_buf,
    size_t tx_cap,
    uint8_t *server_reply_buf,
    size_t server_reply_cap
);

/* Register a service request handler */
int srr_endpoint_register_service(
    srr_endpoint_t *ep,
    uint16_t service_id,
    srr_service_handler_t handler,
    void *user_data
);

/* Register an asynchronous pulse handler */
int srr_endpoint_register_pulse_handler(
    srr_endpoint_t *ep,
    srr_pulse_handler_t handler,
    void *user_data
);

/*
 * Feed a single byte from UART/bus into the endpoint.
 * If a complete frame arrives, automatically dispatches request or updates transaction.
 */
srr_parser_status_t srr_endpoint_feed_byte(srr_endpoint_t *ep, uint8_t byte);

/* Feed a chunk of bytes */
void srr_endpoint_feed_bytes(srr_endpoint_t *ep, const uint8_t *data, size_t len);

/* Advance time for transaction timeouts using caller's monotonic clock */
void srr_endpoint_tick(srr_endpoint_t *ep, uint32_t now_ms);

/*
 * Send an asynchronous event / pulse from the embedded device
 */
int srr_endpoint_send_pulse(
    srr_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len
);

/*
 * Client API: Initiate an asynchronous request from the embedded endpoint.
 * Returns transaction ID (> 0) or 0 on failure (e.g. no slots available).
 */
uint64_t srr_begin_request(
    srr_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    uint8_t *reply_buf,
    size_t reply_cap,
    uint32_t timeout_ms,
    uint32_t now_ms
);

/* Poll transaction state */
srr_trans_state_t srr_poll_transaction(srr_endpoint_t *ep, uint64_t trans_id);

/* Collect reply from completed transaction and free its slot */
int srr_collect_reply(
    srr_endpoint_t *ep,
    uint64_t trans_id,
    size_t *out_reply_len,
    int *out_error_code
);

/* Cancel pending transaction and free its slot */
int srr_cancel_transaction(srr_endpoint_t *ep, uint64_t trans_id);

#ifdef __cplusplus
}
#endif

#endif /* SRR_EMBEDDED_H */
