#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "srr/srr_wire.h"

static void test_crc16_vectors(void) {
    printf("[TEST] CRC-16-CCITT test vectors... ");
    const char *v1 = "123456789";
    uint16_t c1 = srr_crc16((const uint8_t *)v1, strlen(v1), 0xFFFF);
    assert(c1 == 0x29B1);

    const char *v2 = "SRR1";
    uint16_t c2 = srr_crc16((const uint8_t *)v2, strlen(v2), 0xFFFF);
    assert(c2 == 0x1426);
    printf("PASS\n");
}

static void test_cobs_roundtrip(void) {
    printf("[TEST] COBS encode/decode round trips... ");
    uint8_t raw[256];
    uint8_t enc[300];
    uint8_t dec[256];

    /* 1. All zeros */
    memset(raw, 0, 64);
    size_t elen = srr_cobs_encode(raw, 64, enc, sizeof(enc));
    assert(elen > 0);
    size_t dlen = srr_cobs_decode(enc, elen, dec, sizeof(dec));
    assert(dlen == 64);
    assert(memcmp(raw, dec, 64) == 0);

    /* 2. No zeros (1 to 100) */
    for (int i = 0; i < 100; i++) raw[i] = (uint8_t)(i + 1);
    elen = srr_cobs_encode(raw, 100, enc, sizeof(enc));
    dlen = srr_cobs_decode(enc, elen, dec, sizeof(dec));
    assert(dlen == 100);
    assert(memcmp(raw, dec, 100) == 0);

    /* 3. Mixed patterns */
    uint8_t mixed[] = { 0, 1, 0, 0, 2, 3, 4, 0, 5, 0 };
    elen = srr_cobs_encode(mixed, sizeof(mixed), enc, sizeof(enc));
    dlen = srr_cobs_decode(enc, elen, dec, sizeof(dec));
    assert(dlen == sizeof(mixed));
    assert(memcmp(mixed, dec, sizeof(mixed)) == 0);

    printf("PASS\n");
}

static void test_wire_frame_encoding_and_chunking(void) {
    printf("[TEST] Wire frame encoding & streaming parser (1-byte chunking)... ");

    srr_wire_header_t tx_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .flags = 0,
        .service_id = 42,
        .operation_id = 7,
        .reserved = 0,
        .session_id = 1001,
        .trans_id = 999999ULL,
        .payload_len = 0
    };

    const char *payload = "hello_embedded_srr_wire_protocol";
    size_t payload_len = strlen(payload) + 1;

    uint8_t frame_buf[256];
    size_t frame_len = srr_wire_encode_frame(&tx_hdr, payload, payload_len, frame_buf, sizeof(frame_buf));
    assert(frame_len > 0);
    assert(frame_buf[frame_len - 1] == 0x00); /* Must end with 0x00 delimiter */

    /* Parser buffers (statically sized for embedded profile) */
    uint8_t rx_buf[256];
    uint8_t decode_buf[256];
    srr_wire_parser_t parser;
    srr_wire_parser_init(&parser, rx_buf, sizeof(rx_buf), decode_buf, sizeof(decode_buf));

    srr_wire_header_t rx_hdr;
    const uint8_t *rx_payload = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;

    /* Feed 1 byte at a time */
    for (size_t i = 0; i < frame_len; i++) {
        st = srr_wire_parser_feed_byte(&parser, frame_buf[i], &rx_hdr, &rx_payload);
        if (i < frame_len - 1) {
            assert(st == SRR_PARSER_NEED_MORE);
        }
    }

    assert(st == SRR_PARSER_FRAME_READY);
    assert(rx_hdr.service_id == 42);
    assert(rx_hdr.operation_id == 7);
    assert(rx_hdr.session_id == 1001);
    assert(rx_hdr.trans_id == 999999ULL);
    assert(rx_hdr.payload_len == payload_len);
    assert(rx_payload != NULL);
    assert(strcmp((const char *)rx_payload, payload) == 0);

    printf("PASS\n");
}

static void test_crc_corruption_detection(void) {
    printf("[TEST] Corrupted frame rejection via CRC-16... ");

    srr_wire_header_t tx_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REPLY,
        .flags = 0,
        .service_id = 1,
        .operation_id = 1,
        .reserved = 0,
        .session_id = 123,
        .trans_id = 456,
        .payload_len = 0
    };

    uint8_t frame_buf[128];
    size_t frame_len = srr_wire_encode_frame(&tx_hdr, "valid_data", 11, frame_buf, sizeof(frame_buf));
    assert(frame_len > 0);

    /* Corrupt byte 5 (flip a bit) */
    frame_buf[5] ^= 0x40;

    uint8_t rx_buf[128], decode_buf[128];
    srr_wire_parser_t parser;
    srr_wire_parser_init(&parser, rx_buf, sizeof(rx_buf), decode_buf, sizeof(decode_buf));

    srr_wire_header_t rx_hdr;
    const uint8_t *rx_payload = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;

    for (size_t i = 0; i < frame_len; i++) {
        st = srr_wire_parser_feed_byte(&parser, frame_buf[i], &rx_hdr, &rx_payload);
    }

    /* Must reject due to CRC mismatch */
    assert(st == SRR_PARSER_ERR_CRC);
    printf("PASS\n");
}

static void test_arbitrary_chunking_and_multiple_frames(void) {
    printf("[TEST] Arbitrary chunk boundaries and back-to-back frames... ");

    srr_wire_header_t h1 = { .service_id = 1, .trans_id = 101, .msg_type = SRR_WIRE_REQUEST };
    srr_wire_header_t h2 = { .service_id = 2, .trans_id = 102, .msg_type = SRR_WIRE_PULSE };

    uint8_t stream[512];
    size_t len1 = srr_wire_encode_frame(&h1, "frame1", 7, stream, sizeof(stream));
    size_t len2 = srr_wire_encode_frame(&h2, "frame2", 7, stream + len1, sizeof(stream) - len1);
    size_t total_len = len1 + len2;

    uint8_t rx_buf[256], decode_buf[256];
    srr_wire_parser_t parser;
    srr_wire_parser_init(&parser, rx_buf, sizeof(rx_buf), decode_buf, sizeof(decode_buf));

    int frames_found = 0;
    /* Feed in arbitrary 7-byte chunks */
    size_t pos = 0;
    while (pos < total_len) {
        size_t chunk_sz = (total_len - pos < 7) ? (total_len - pos) : 7;
        for (size_t c = 0; c < chunk_sz; c++) {
            srr_wire_header_t out_hdr;
            const uint8_t *out_pl = NULL;
            srr_parser_status_t st = srr_wire_parser_feed_byte(&parser, stream[pos + c], &out_hdr, &out_pl);
            if (st == SRR_PARSER_FRAME_READY) {
                frames_found++;
                if (frames_found == 1) {
                    assert(out_hdr.trans_id == 101);
                    assert(strcmp((const char *)out_pl, "frame1") == 0);
                } else if (frames_found == 2) {
                    assert(out_hdr.trans_id == 102);
                    assert(strcmp((const char *)out_pl, "frame2") == 0);
                }
            }
        }
        pos += chunk_sz;
    }

    assert(frames_found == 2);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Wire Protocol & COBS Tests ===\n");
    test_crc16_vectors();
    test_cobs_roundtrip();
    test_wire_frame_encoding_and_chunking();
    test_crc_corruption_detection();
    test_arbitrary_chunking_and_multiple_frames();
    printf("All wire protocol tests passed!\n");
    return 0;
}
