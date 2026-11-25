#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "srr/srr_wire.h"
#include "../../src/srr_proto.h"

static void hex_dump(FILE *f, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        fprintf(f, "%02x", data[i]);
    }
}

int main(void) {
    FILE *f = fopen("tests/conformance/vectors.json", "w");
    if (!f) {
        perror("fopen");
        return 1;
    }

    fprintf(f, "{\n");

    /* 1. CRC16 Vectors */
    fprintf(f, "  \"crc16\": [\n");
    struct {
        const char *name;
        const char *data;
        size_t len;
    } crc_tests[] = {
        {"123456789", "123456789", 9},
        {"SRR1", "SRR1", 4},
        {"empty", "", 0},
        {"four_zeros", "\x00\x00\x00\x00", 4},
        {"pattern_55aa", "\x55\xaa\x55\xaa", 4}
    };
    for (size_t i = 0; i < sizeof(crc_tests)/sizeof(crc_tests[0]); i++) {
        uint16_t c = srr_crc16((const uint8_t *)crc_tests[i].data, crc_tests[i].len, 0xFFFF);
        fprintf(f, "    {\"name\": \"%s\", \"hex_input\": \"", crc_tests[i].name);
        hex_dump(f, (const uint8_t *)crc_tests[i].data, crc_tests[i].len);
        fprintf(f, "\", \"expected_crc\": %u}%s\n", c, (i + 1 == sizeof(crc_tests)/sizeof(crc_tests[0])) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    /* 2. COBS Vectors */
    fprintf(f, "  \"cobs\": [\n");
    struct {
        const char *name;
        const uint8_t *data;
        size_t len;
    } cobs_tests[] = {
        {"single_zero", (const uint8_t *)"\x00", 1},
        {"two_zeros", (const uint8_t *)"\x00\x00", 2},
        {"digits", (const uint8_t *)"\x01\x02\x03\x04\x05", 5},
        {"mixed", (const uint8_t *)"\x00\x01\x00\x00\x02\x03\x04\x00\x05\x00", 10}
    };
    for (size_t i = 0; i < sizeof(cobs_tests)/sizeof(cobs_tests[0]); i++) {
        uint8_t enc[64];
        size_t elen = srr_cobs_encode(cobs_tests[i].data, cobs_tests[i].len, enc, sizeof(enc));
        fprintf(f, "    {\"name\": \"%s\", \"raw_hex\": \"", cobs_tests[i].name);
        hex_dump(f, cobs_tests[i].data, cobs_tests[i].len);
        fprintf(f, "\", \"encoded_hex\": \"");
        hex_dump(f, enc, elen);
        fprintf(f, "\"}%s\n", (i + 1 == sizeof(cobs_tests)/sizeof(cobs_tests[0])) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    /* 3. 24-byte IPC Frames */
    fprintf(f, "  \"ipc_frames\": [\n");

    /* 3a: Request */
    {
        uint8_t buf[64];
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_REQUEST,
            .flags = 0,
            .trans_id = 42,
            .payload_len = 4
        };
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), "PING", 4);
        fprintf(f, "    {\"name\": \"ipc_request_ping\", \"type\": \"request\", \"trans_id\": 42, \"hex\": \"");
        hex_dump(f, buf, sizeof(hdr) + 4);
        fprintf(f, "\"},\n");
    }

    /* 3b: Reply */
    {
        uint8_t buf[64];
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_REPLY,
            .flags = 0,
            .trans_id = 42,
            .payload_len = 4
        };
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), "PONG", 4);
        fprintf(f, "    {\"name\": \"ipc_reply_pong\", \"type\": \"reply\", \"trans_id\": 42, \"hex\": \"");
        hex_dump(f, buf, sizeof(hdr) + 4);
        fprintf(f, "\"},\n");
    }

    /* 3c: Pulse */
    {
        uint8_t buf[64];
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_PULSE,
            .flags = 0,
            .trans_id = 0,
            .payload_len = sizeof(srr_pulse_payload_t)
        };
        srr_pulse_payload_t pl = {
            .code = 1,
            ._reserved = 0,
            .value = 123456789ULL
        };
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), &pl, sizeof(pl));
        fprintf(f, "    {\"name\": \"ipc_pulse_1\", \"type\": \"pulse\", \"code\": 1, \"value\": 123456789, \"hex\": \"");
        hex_dump(f, buf, sizeof(hdr) + sizeof(pl));
        fprintf(f, "\"},\n");
    }

    /* 3d: Error */
    {
        uint8_t buf[64];
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_ERROR,
            .flags = 0,
            .trans_id = 42,
            .payload_len = sizeof(srr_error_payload_t)
        };
        srr_error_payload_t err = {
            .error_status = 2, /* SRR_CANCELLED */
            ._reserved = 0
        };
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), &err, sizeof(err));
        fprintf(f, "    {\"name\": \"ipc_error_cancelled\", \"type\": \"error\", \"trans_id\": 42, \"status\": 2, \"hex\": \"");
        hex_dump(f, buf, sizeof(hdr) + sizeof(err));
        fprintf(f, "\"},\n");
    }

    /* 3e: Goodbye */
    {
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_GOODBYE,
            .flags = 0,
            .trans_id = 0,
            .payload_len = 0
        };
        fprintf(f, "    {\"name\": \"ipc_goodbye\", \"type\": \"goodbye\", \"hex\": \"");
        hex_dump(f, (const uint8_t *)&hdr, sizeof(hdr));
        fprintf(f, "\"}\n");
    }
    fprintf(f, "  ],\n");

    /* 4. 32-byte Wire Frames (Unencoded & COBS-Encoded with 0x00 delimiter) */
    fprintf(f, "  \"wire_frames\": [\n");

    /* 4a: Wire Request */
    {
        srr_wire_header_t hdr = {
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
        const char *pl = "hello_srr";
        uint8_t frame[128];
        size_t flen = srr_wire_encode_frame(&hdr, pl, strlen(pl), frame, sizeof(frame));
        fprintf(f, "    {\"name\": \"wire_request\", \"service_id\": 42, \"op_id\": 7, \"session_id\": 1001, \"trans_id\": 999999, \"cobs_delimited_hex\": \"");
        hex_dump(f, frame, flen);
        fprintf(f, "\"},\n");
    }

    /* 4b: Wire Reply */
    {
        srr_wire_header_t hdr = {
            .magic = SRR_WIRE_MAGIC,
            .version = SRR_WIRE_VERSION,
            .msg_type = SRR_WIRE_REPLY,
            .flags = 0,
            .service_id = 42,
            .operation_id = 7,
            .reserved = 0,
            .session_id = 1001,
            .trans_id = 999999ULL,
            .payload_len = 0
        };
        const char *pl = "world";
        uint8_t frame[128];
        size_t flen = srr_wire_encode_frame(&hdr, pl, strlen(pl), frame, sizeof(frame));
        fprintf(f, "    {\"name\": \"wire_reply\", \"service_id\": 42, \"op_id\": 7, \"session_id\": 1001, \"trans_id\": 999999, \"cobs_delimited_hex\": \"");
        hex_dump(f, frame, flen);
        fprintf(f, "\"},\n");
    }

    /* 4c: Wire Error */
    {
        srr_wire_header_t hdr = {
            .magic = SRR_WIRE_MAGIC,
            .version = SRR_WIRE_VERSION,
            .msg_type = SRR_WIRE_ERROR,
            .flags = 7, /* SRR_TOO_LARGE */
            .service_id = 42,
            .operation_id = 7,
            .reserved = 0,
            .session_id = 1001,
            .trans_id = 999999ULL,
            .payload_len = 0
        };
        uint8_t frame[128];
        size_t flen = srr_wire_encode_frame(&hdr, NULL, 0, frame, sizeof(frame));
        fprintf(f, "    {\"name\": \"wire_error\", \"service_id\": 42, \"op_id\": 7, \"session_id\": 1001, \"trans_id\": 999999, \"flags\": 7, \"cobs_delimited_hex\": \"");
        hex_dump(f, frame, flen);
        fprintf(f, "\"}\n");
    }
    fprintf(f, "  ]\n");

    fprintf(f, "}\n");
    fclose(f);
    printf("Wrote tests/conformance/vectors.json successfully.\n");
    return 0;
}
