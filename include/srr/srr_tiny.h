#ifndef SRR_TINY_H
#define SRR_TINY_H

#include "srr_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR Tiny Profile (AVR ATmega328P / 8-Bit Ultra-Constrained Profile)
 * ============================================================================
 * Design targets:
 *   - ATmega328P (2 KB SRAM, 32 KB Flash, 16 MHz).
 *   - Zero dynamic memory allocation.
 *   - Strict static RAM footprint < 250 bytes total.
 *   - Payload limited to 32 bytes max (sufficient for sensor readings, GPIO, telemetry).
 *   - Single outstanding transaction slot (Tiny profile requirement).
 *   - 100% interoperable with standard SRR wire frames and desktop clients.
 */

#define SRR_TINY_MAX_PAYLOAD     32
#define SRR_TINY_RAW_MAX         (SRR_HEADER_SIZE + SRR_TINY_MAX_PAYLOAD + SRR_CRC_SIZE) /* 66 bytes */
#define SRR_TINY_COBS_MAX        (SRR_TINY_RAW_MAX + (SRR_TINY_RAW_MAX / 254) + 2)       /* 68 bytes */
#define SRR_TINY_MAX_SERVICES    4

typedef int (*srr_tiny_handler_t)(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *req,
    uint8_t req_len,
    uint8_t *reply,
    uint8_t reply_cap,
    uint8_t *out_reply_len,
    void *user_data
);

typedef int (*srr_tiny_tx_fn_t)(const uint8_t *data, size_t len, void *user_data);

typedef struct {
    uint16_t service_id;
    srr_tiny_handler_t handler;
    void *user_data;
} srr_tiny_service_t;

typedef enum {
    SRR_TINY_STATE_IDLE = 0,
    SRR_TINY_STATE_PENDING,
    SRR_TINY_STATE_REPLIED,
    SRR_TINY_STATE_ERROR
} srr_tiny_trans_state_t;

typedef struct {
    uint32_t session_id;
    srr_tiny_tx_fn_t tx_fn;
    void *tx_user_data;

    /* Preallocated static buffers for zero heap allocation */
    uint8_t rx_buf[SRR_TINY_COBS_MAX + 4];
    uint8_t decode_buf[SRR_TINY_RAW_MAX + 4];
    uint8_t tx_buf[SRR_TINY_COBS_MAX + 4];
    uint8_t server_rep_buf[SRR_TINY_MAX_PAYLOAD];

    /* Streaming parser */
    srr_wire_parser_t parser;

    /* Service table (up to 4 services) */
    srr_tiny_service_t services[SRR_TINY_MAX_SERVICES];
    uint8_t service_count;

    /* Single client transaction slot */
    uint64_t client_trans_id;
    srr_tiny_trans_state_t client_state;
    uint8_t client_reply_buf[SRR_TINY_MAX_PAYLOAD];
    uint8_t client_reply_len;
    int client_error;
} srr_tiny_endpoint_t;

/* Initialize tiny endpoint */
void srr_tiny_init(
    srr_tiny_endpoint_t *ep,
    uint32_t session_id,
    srr_tiny_tx_fn_t tx_fn,
    void *tx_user_data
);

/* Register a service */
int srr_tiny_register_service(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    srr_tiny_handler_t handler,
    void *user_data
);

/* Feed one byte from UART ISR or ring buffer into tiny endpoint */
srr_parser_status_t srr_tiny_feed_byte(srr_tiny_endpoint_t *ep, uint8_t byte);

/* Send an asynchronous pulse from the AVR device */
int srr_tiny_send_pulse(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    uint8_t payload_len
);

/*
 * Client operations: Initiate a request from AVR to host.
 * Returns transaction ID (> 0) or 0 if busy.
 */
uint64_t srr_tiny_begin_request(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    uint8_t req_len
);

/* Poll for reply of pending request. Returns true if completed, false if still pending. */
bool srr_tiny_poll_reply(
    srr_tiny_endpoint_t *ep,
    uint64_t trans_id,
    uint8_t *out_reply,
    uint8_t reply_cap,
    uint8_t *out_reply_len,
    int *out_error
);

#ifdef __cplusplus
}
#endif

#endif /* SRR_TINY_H */
