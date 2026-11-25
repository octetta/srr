#include "srr_p1_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SVC_MOTOR 10
#define OP_SET_SPEED 1

static void test_p1_multi_cog_roundtrip(void) {
    printf("Testing Parallax Propeller P1 Multi-Cog Hub RAM Round-Trip...\n");

    srr_p1_hub_t hub;
    srr_p1_hub_init(&hub, 0x5031, 0);

    /* 1. External host sends request frame into Hub RX ring */
    srr_wire_header_t req_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x5031,
        .trans_id = 77,
        .service_id = SVC_MOTOR,
        .operation_id = OP_SET_SPEED,
        .payload_len = 2
    };
    uint8_t req_payload[2] = { 0x01, 0x80 }; /* Motor 1, 128 RPM */
    uint8_t wire_frame[128];
    size_t frame_len = srr_wire_encode_frame(&req_hdr, req_payload, 2, wire_frame, sizeof(wire_frame));
    assert(frame_len > 0);

    /* Push bytes into hub.rx_ring (simulating Cog 0 UART RX assembly) */
    for (size_t i = 0; i < frame_len; i++) {
        hub.rx_ring[hub.rx_head] = wire_frame[i];
        hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
    }

    /* Cog 0 executes step: parses frame, loads into mailbox */
    int actions = srr_p1_io_cog_step(&hub);
    assert(actions > 0);
    assert(hub.mailbox.state == SRR_P1_MBOX_REQUEST_READY);
    assert(hub.mailbox.service_id == SVC_MOTOR);

    /* Worker Cog 1 polls and claims request */
    uint16_t op = 0;
    uint64_t tid = 0;
    uint8_t payload[64];
    uint8_t len = 0;

    bool claimed = srr_p1_worker_claim(&hub, 1, SVC_MOTOR, &op, &tid, payload, &len);
    assert(claimed == true);
    assert(op == OP_SET_SPEED);
    assert(tid == 77);
    assert(len == 2);
    assert(payload[0] == 0x01 && payload[1] == 0x80);

    /* Worker Cog 1 completes motor command and writes reply */
    uint8_t reply_payload[4] = { 'O', 'K', '8', '0' };
    int rc = srr_p1_worker_reply(&hub, 1, tid, reply_payload, 4, false);
    assert(rc == 0);
    assert(hub.mailbox.state == SRR_P1_MBOX_REPLY_READY);

    /* Cog 0 executes step: encodes reply into TX ring */
    actions = srr_p1_io_cog_step(&hub);
    assert(actions > 0);
    assert(hub.mailbox.state == SRR_P1_MBOX_EMPTY);
    assert(hub.tx_head != hub.tx_tail);

    /* Verify wire frame produced in tx_ring */
    srr_wire_parser_t client_p;
    uint8_t crx[128], cdec[128];
    srr_wire_parser_init(&client_p, crx, sizeof(crx), cdec, sizeof(cdec));

    srr_wire_header_t out_rep_hdr;
    const uint8_t *out_rep_payload = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;

    while (hub.tx_tail != hub.tx_head) {
        uint8_t b = hub.tx_ring[hub.tx_tail];
        hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;
        st = srr_wire_parser_feed_byte(&client_p, b, &out_rep_hdr, &out_rep_payload);
    }

    assert(st == SRR_PARSER_FRAME_READY);
    assert(out_rep_hdr.msg_type == SRR_WIRE_REPLY);
    assert(out_rep_hdr.trans_id == 77);
    assert(out_rep_hdr.payload_len == 4);
    assert(memcmp(out_rep_payload, "OK80", 4) == 0);

    printf("  P1 Multi-Cog round-trip verified: 'OK80' reply matched.\n");
}

static void test_p1_cog_pulse(void) {
    printf("Testing Parallax Propeller P1 Cog Pulse Generation...\n");

    srr_p1_hub_t hub;
    srr_p1_hub_init(&hub, 0x5031, 0);

    /* Cog 2 emits an emergency motor halt pulse directly into Hub TX ring */
    int rc = srr_p1_emit_pulse(&hub, SVC_MOTOR, 99, "HALT", 4);
    assert(rc == 0);
    assert(hub.tx_head != hub.tx_tail);

    /* Client decodes pulse from TX ring */
    srr_wire_parser_t client_p;
    uint8_t crx[128], cdec[128];
    srr_wire_parser_init(&client_p, crx, sizeof(crx), cdec, sizeof(cdec));

    srr_wire_header_t pulse_hdr;
    const uint8_t *pulse_payload = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;

    while (hub.tx_tail != hub.tx_head) {
        uint8_t b = hub.tx_ring[hub.tx_tail];
        hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;
        st = srr_wire_parser_feed_byte(&client_p, b, &pulse_hdr, &pulse_payload);
    }

    assert(st == SRR_PARSER_FRAME_READY);
    assert(pulse_hdr.msg_type == SRR_WIRE_PULSE);
    assert(pulse_hdr.service_id == SVC_MOTOR);
    assert(pulse_hdr.operation_id == 99);
    assert(pulse_hdr.payload_len == 4);
    assert(memcmp(pulse_payload, "HALT", 4) == 0);

    printf("  P1 Cog Pulse verified successfully.\n");
}

int main(void) {
    printf("=========================================\n");
    printf("SRR Parallax Propeller P1 Multi-Cog Tests\n");
    printf("=========================================\n");

    test_p1_multi_cog_roundtrip();
    test_p1_cog_pulse();

    printf("\nAll Parallax Propeller P1 tests PASSED!\n");
    return 0;
}
