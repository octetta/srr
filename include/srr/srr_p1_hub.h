#ifndef SRR_P1_HUB_H
#define SRR_P1_HUB_H

#include "srr_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Parallax Propeller P1 Multi-Cog Shared Hub RAM Architecture
 * ============================================================================
 * Design targets:
 *   - Parallax P8X32A (8 Cogs @ 80 MHz, 32 KB Hub RAM, 8 Hardware Locks).
 *   - Zero conventional OS / interrupt model.
 *   - Dedicated I/O Cog (Cog 0) streaming UART RX/TX into circular Hub buffers.
 *   - Worker Cogs (Cogs 1..7) executing service requests arbitrated via Hub locks.
 *   - Standard SRR wire framing (COBS + CRC-16) across the physical pins.
 */

#define SRR_P1_RING_SIZE       256
#define SRR_P1_MAX_PAYLOAD     64

typedef enum {
    SRR_P1_MBOX_EMPTY = 0,
    SRR_P1_MBOX_REQUEST_READY,
    SRR_P1_MBOX_PROCESSING,
    SRR_P1_MBOX_REPLY_READY,
    SRR_P1_MBOX_ERROR
} srr_p1_mbox_state_t;

/* Hub RAM Request/Reply Mailbox */
typedef struct {
    volatile uint32_t state;        /* srr_p1_mbox_state_t */
    uint16_t          service_id;
    uint16_t          operation_id;
    uint64_t          trans_id;
    uint8_t           payload_len;
    uint8_t           payload[SRR_P1_MAX_PAYLOAD];
    uint8_t           reply_len;
    uint8_t           reply[SRR_P1_MAX_PAYLOAD];
    uint8_t           assigned_cog; /* Cog ID processing this transaction */
} srr_p1_mailbox_t;

/* Shared Hub RAM layout (statically allocated at known Hub address) */
typedef struct {
    uint32_t          magic;        /* SRR_WIRE_MAGIC */
    uint32_t          session_id;
    uint8_t           lock_id;      /* Hardware lock 0..7 */

    /* UART RX Circular Buffer in Hub RAM */
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    uint8_t           rx_ring[SRR_P1_RING_SIZE];

    /* UART TX Circular Buffer in Hub RAM */
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    uint8_t           tx_ring[SRR_P1_RING_SIZE];

    /* Work Mailbox for Cog Dispatch */
    srr_p1_mailbox_t  mailbox;

    /* Parser scratchpad */
    srr_wire_parser_t parser;
    uint8_t           parser_rx[128];
    uint8_t           parser_dec[128];
    uint8_t           tx_scratch[128];

    /* Statistics */
    volatile uint32_t rx_frames;
    volatile uint32_t tx_frames;
} srr_p1_hub_t;

/* Initialize Hub RAM structures */
void srr_p1_hub_init(srr_p1_hub_t *hub, uint32_t session_id, uint8_t lock_id);

/*
 * I/O Cog (Cog 0) execution step:
 * Drains RX ring buffer, feeds streaming wire parser, populates mailbox when
 * a frame is ready, and transmits completed replies from the TX ring buffer.
 */
int srr_p1_io_cog_step(srr_p1_hub_t *hub);

/*
 * Worker Cog (Cogs 1..7) poll:
 * Checks if a request matching service_id is waiting in Hub mailbox.
 * Returns true if request claimed for processing, false if idle.
 */
bool srr_p1_worker_claim(
    srr_p1_hub_t *hub,
    uint8_t cog_id,
    uint16_t service_id,
    uint16_t *out_op,
    uint64_t *out_trans_id,
    uint8_t *out_payload,
    uint8_t *out_len
);

/*
 * Worker Cog completes request:
 * Writes reply payload into Hub mailbox and signals I/O cog to transmit.
 */
int srr_p1_worker_reply(
    srr_p1_hub_t *hub,
    uint8_t cog_id,
    uint64_t trans_id,
    const void *reply,
    uint8_t reply_len,
    bool is_error
);

/*
 * Any Cog can emit an asynchronous pulse directly into Hub TX queue.
 */
int srr_p1_emit_pulse(
    srr_p1_hub_t *hub,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    uint8_t payload_len
);

#ifdef __cplusplus
}
#endif

#endif /* SRR_P1_HUB_H */
