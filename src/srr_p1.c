#include "srr_p1_hub.h"
#include <string.h>

void srr_p1_hub_init(srr_p1_hub_t *hub, uint32_t session_id, uint8_t lock_id) {
    if (!hub) return;
    memset(hub, 0, sizeof(srr_p1_hub_t));
    hub->magic = SRR_WIRE_MAGIC;
    hub->session_id = session_id;
    hub->lock_id = lock_id;

    srr_wire_parser_init(
        &hub->parser,
        hub->parser_rx, sizeof(hub->parser_rx),
        hub->parser_dec, sizeof(hub->parser_dec)
    );
}

int srr_p1_io_cog_step(srr_p1_hub_t *hub) {
    if (!hub) return -1;
    int actions = 0;

    /* 1. Process pending bytes from RX circular buffer */
    while (hub->rx_tail != hub->rx_head) {
        uint8_t byte = hub->rx_ring[hub->rx_tail];
        hub->rx_tail = (hub->rx_tail + 1) % SRR_P1_RING_SIZE;
        actions++;

        srr_wire_header_t in_hdr;
        const uint8_t *in_payload = NULL;

        srr_parser_status_t st = srr_wire_parser_feed_byte(&hub->parser, byte, &in_hdr, &in_payload);
        if (st == SRR_PARSER_FRAME_READY) {
            hub->rx_frames++;

            if (in_hdr.magic != SRR_WIRE_MAGIC || in_hdr.version != SRR_WIRE_VERSION) {
                continue;
            }

            if (in_hdr.msg_type == SRR_WIRE_REQUEST) {
                if (hub->mailbox.state == SRR_P1_MBOX_EMPTY) {
                    hub->mailbox.service_id = in_hdr.service_id;
                    hub->mailbox.operation_id = in_hdr.operation_id;
                    hub->mailbox.trans_id = in_hdr.trans_id;
                    uint8_t copy_len = (uint8_t)(in_hdr.payload_len < SRR_P1_MAX_PAYLOAD ? in_hdr.payload_len : SRR_P1_MAX_PAYLOAD);
                    if (in_payload && copy_len > 0) {
                        memcpy(hub->mailbox.payload, in_payload, copy_len);
                    }
                    hub->mailbox.payload_len = copy_len;
                    hub->mailbox.assigned_cog = 0;
                    hub->mailbox.state = SRR_P1_MBOX_REQUEST_READY;
                } else {
                    /* Mailbox congested: emit immediate error frame */
                    srr_wire_header_t err_hdr = {
                        .magic = SRR_WIRE_MAGIC,
                        .version = SRR_WIRE_VERSION,
                        .msg_type = SRR_WIRE_ERROR,
                        .session_id = hub->session_id,
                        .trans_id = in_hdr.trans_id,
                        .service_id = in_hdr.service_id,
                        .operation_id = in_hdr.operation_id,
                        .payload_len = 0
                    };
                    size_t enc = srr_wire_encode_frame(&err_hdr, NULL, 0, hub->tx_scratch, sizeof(hub->tx_scratch));
                    for (size_t i = 0; i < enc; i++) {
                        uint32_t next = (hub->tx_head + 1) % SRR_P1_RING_SIZE;
                        if (next != hub->tx_tail) {
                            hub->tx_ring[hub->tx_head] = hub->tx_scratch[i];
                            hub->tx_head = next;
                        }
                    }
                }
            }
        }
    }

    /* 2. Check if a worker cog has placed a completed reply into the mailbox */
    if (hub->mailbox.state == SRR_P1_MBOX_REPLY_READY || hub->mailbox.state == SRR_P1_MBOX_ERROR) {
        srr_wire_header_t rep_hdr = {
            .magic = SRR_WIRE_MAGIC,
            .version = SRR_WIRE_VERSION,
            .msg_type = (hub->mailbox.state == SRR_P1_MBOX_REPLY_READY) ? SRR_WIRE_REPLY : SRR_WIRE_ERROR,
            .session_id = hub->session_id,
            .trans_id = hub->mailbox.trans_id,
            .service_id = hub->mailbox.service_id,
            .operation_id = hub->mailbox.operation_id,
            .payload_len = (hub->mailbox.state == SRR_P1_MBOX_REPLY_READY) ? hub->mailbox.reply_len : 0
        };

        size_t enc = srr_wire_encode_frame(
            &rep_hdr,
            (hub->mailbox.state == SRR_P1_MBOX_REPLY_READY) ? hub->mailbox.reply : NULL,
            rep_hdr.payload_len,
            hub->tx_scratch,
            sizeof(hub->tx_scratch)
        );

        /* Push encoded frame into TX ring */
        for (size_t i = 0; i < enc; i++) {
            uint32_t next = (hub->tx_head + 1) % SRR_P1_RING_SIZE;
            if (next != hub->tx_tail) {
                hub->tx_ring[hub->tx_head] = hub->tx_scratch[i];
                hub->tx_head = next;
            }
        }

        hub->tx_frames++;
        hub->mailbox.state = SRR_P1_MBOX_EMPTY;
        actions++;
    }

    return actions;
}

bool srr_p1_worker_claim(
    srr_p1_hub_t *hub,
    uint8_t cog_id,
    uint16_t service_id,
    uint16_t *out_op,
    uint64_t *out_trans_id,
    uint8_t *out_payload,
    uint8_t *out_len)
{
    if (!hub) return false;

    if (hub->mailbox.state == SRR_P1_MBOX_REQUEST_READY && hub->mailbox.service_id == service_id) {
        hub->mailbox.state = SRR_P1_MBOX_PROCESSING;
        hub->mailbox.assigned_cog = cog_id;

        if (out_op) *out_op = hub->mailbox.operation_id;
        if (out_trans_id) *out_trans_id = hub->mailbox.trans_id;
        if (out_payload && hub->mailbox.payload_len > 0) {
            memcpy(out_payload, hub->mailbox.payload, hub->mailbox.payload_len);
        }
        if (out_len) *out_len = hub->mailbox.payload_len;
        return true;
    }
    return false;
}

int srr_p1_worker_reply(
    srr_p1_hub_t *hub,
    uint8_t cog_id,
    uint64_t trans_id,
    const void *reply,
    uint8_t reply_len,
    bool is_error)
{
    if (!hub) return -1;
    if (hub->mailbox.state != SRR_P1_MBOX_PROCESSING ||
        hub->mailbox.assigned_cog != cog_id ||
        hub->mailbox.trans_id != trans_id)
    {
        return -1;
    }

    if (is_error) {
        hub->mailbox.state = SRR_P1_MBOX_ERROR;
        return 0;
    }

    uint8_t copy_len = reply_len < SRR_P1_MAX_PAYLOAD ? reply_len : SRR_P1_MAX_PAYLOAD;
    if (reply && copy_len > 0) {
        memcpy(hub->mailbox.reply, reply, copy_len);
    }
    hub->mailbox.reply_len = copy_len;
    hub->mailbox.state = SRR_P1_MBOX_REPLY_READY;
    return 0;
}

int srr_p1_emit_pulse(
    srr_p1_hub_t *hub,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    uint8_t payload_len)
{
    if (!hub) return -1;

    srr_wire_header_t hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_PULSE,
        .session_id = hub->session_id,
        .trans_id = 0,
        .service_id = service_id,
        .operation_id = operation_id,
        .payload_len = payload_len
    };

    uint8_t scratch[128];
    size_t enc = srr_wire_encode_frame(&hdr, payload, payload_len, scratch, sizeof(scratch));
    if (enc == 0) return -1;

    for (size_t i = 0; i < enc; i++) {
        uint32_t next = (hub->tx_head + 1) % SRR_P1_RING_SIZE;
        if (next != hub->tx_tail) {
            hub->tx_ring[hub->tx_head] = scratch[i];
            hub->tx_head = next;
        } else {
            return -1; /* TX ring full */
        }
    }

    hub->tx_frames++;
    return 0;
}
