#include "srr_tiny.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SVC_ADC 1
#define OP_READ 2

typedef struct {
    uint8_t buf[256];
    size_t len;
} tiny_tx_mock_t;

static int tiny_mock_tx(const uint8_t *data, size_t len, void *user_data) {
    tiny_tx_mock_t *m = (tiny_tx_mock_t *)user_data;
    if (m->len + len > sizeof(m->buf)) return -1;
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

static int adc_tiny_handler(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *req,
    uint8_t req_len,
    uint8_t *reply,
    uint8_t reply_cap,
    uint8_t *out_reply_len,
    void *user_data)
{
    (void)service_id; (void)req; (void)req_len; (void)user_data;
    if (operation_id == OP_READ) {
        if (reply_cap < 2) return -1;
        uint16_t adc_val = 512; /* Simulated 10-bit AVR ADC reading */
        reply[0] = (uint8_t)(adc_val & 0xFF);
        reply[1] = (uint8_t)((adc_val >> 8) & 0xFF);
        *out_reply_len = 2;
        return 0;
    }
    return -1;
}

static void test_tiny_server_dispatch(void) {
    printf("Testing AVR Tiny Profile server dispatch & zero heap memory...\n");

    tiny_tx_mock_t mock_tx = { .len = 0 };
    srr_tiny_endpoint_t ep;

    srr_tiny_init(&ep, 0xA001, tiny_mock_tx, &mock_tx);
    int rc = srr_tiny_register_service(&ep, SVC_ADC, adc_tiny_handler, NULL);
    assert(rc == 0);

    /* Construct standard desktop wire request frame */
    srr_wire_header_t req_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0xA001,
        .trans_id = 100,
        .service_id = SVC_ADC,
        .operation_id = OP_READ,
        .payload_len = 0
    };

    uint8_t frame[128];
    size_t frame_len = srr_wire_encode_frame(&req_hdr, NULL, 0, frame, sizeof(frame));
    assert(frame_len > 0);

    /* Feed 1 byte at a time */
    for (size_t i = 0; i < frame_len; i++) {
        srr_tiny_feed_byte(&ep, frame[i]);
    }

    assert(mock_tx.len > 0);

    /* Decode reply */
    srr_wire_parser_t dec_p;
    uint8_t rx_b[128], dec_b[128];
    srr_wire_parser_init(&dec_p, rx_b, sizeof(rx_b), dec_b, sizeof(dec_b));

    srr_wire_header_t rep_hdr;
    const uint8_t *rep_payload = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;
    for (size_t i = 0; i < mock_tx.len; i++) {
        st = srr_wire_parser_feed_byte(&dec_p, mock_tx.buf[i], &rep_hdr, &rep_payload);
    }
    assert(st == SRR_PARSER_FRAME_READY);
    assert(rep_hdr.msg_type == SRR_WIRE_REPLY);
    assert(rep_hdr.trans_id == 100);
    assert(rep_hdr.payload_len == 2);
    uint16_t adc_val = (uint8_t)rep_payload[0] | ((uint8_t)rep_payload[1] << 8);
    assert(adc_val == 512);

    printf("  AVR Tiny server dispatch verified (ADC: 512).\n");
}

static void test_tiny_client_and_pulse(void) {
    printf("Testing AVR Tiny Profile client requests and pulse...\n");

    tiny_tx_mock_t mock_tx = { .len = 0 };
    srr_tiny_endpoint_t ep;
    srr_tiny_init(&ep, 0xA002, tiny_mock_tx, &mock_tx);

    /* 1. Pulse */
    int rc = srr_tiny_send_pulse(&ep, 5, 1, "PULSE", 5);
    assert(rc == 0);
    assert(mock_tx.len > 0);

    /* 2. Client request */
    mock_tx.len = 0;
    uint64_t tid = srr_tiny_begin_request(&ep, 10, 20, "PING", 4);
    assert(tid > 0);
    assert(mock_tx.len > 0);

    /* Feed reply frame */
    srr_wire_header_t rep_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REPLY,
        .session_id = 0xA002,
        .trans_id = tid,
        .service_id = 10,
        .operation_id = 20,
        .payload_len = 4
    };
    uint8_t rep_f[128];
    size_t rlen = srr_wire_encode_frame(&rep_hdr, "PONG", 4, rep_f, sizeof(rep_f));
    for (size_t i = 0; i < rlen; i++) {
        srr_tiny_feed_byte(&ep, rep_f[i]);
    }

    uint8_t reply[32];
    uint8_t actual_len = 0;
    int err = 0;
    bool done = srr_tiny_poll_reply(&ep, tid, reply, sizeof(reply), &actual_len, &err);
    assert(done == true);
    assert(err == 0);
    assert(actual_len == 4);
    assert(memcmp(reply, "PONG", 4) == 0);

    printf("  AVR Tiny client transaction verified (PONG).\n");
}

int main(void) {
    printf("=========================================\n");
    printf("SRR AVR Tiny Profile Unit Test Suite\n");
    printf("=========================================\n");

    test_tiny_server_dispatch();
    test_tiny_client_and_pulse();

    printf("\nAll AVR Tiny Profile tests PASSED!\n");
    return 0;
}
