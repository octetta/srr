#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "srr/srr_wire.h"
#include "../../src/srr_proto.h"

int main(void) {
    /* 1. CRC16 */
    assert(srr_crc16((const uint8_t *)"123456789", 9, 0xFFFF) == 10673);
    assert(srr_crc16((const uint8_t *)"SRR1", 4, 0xFFFF) == 5158);
    assert(srr_crc16((const uint8_t *)"", 0, 0xFFFF) == 65535);

    /* 2. COBS */
    uint8_t raw1[] = {0x00};
    uint8_t enc1[16], dec1[16];
    size_t elen1 = srr_cobs_encode(raw1, 1, enc1, sizeof(enc1));
    assert(elen1 == 2 && enc1[0] == 0x01 && enc1[1] == 0x01);
    size_t dlen1 = srr_cobs_decode(enc1, elen1, dec1, sizeof(dec1));
    assert(dlen1 == 1 && dec1[0] == 0x00);

    /* 3. Wire Request Frame */
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
    assert(flen > 0);
    assert(frame[flen - 1] == 0x00);

    /* Parser verification */
    uint8_t rx_buf[128], decode_buf[128];
    srr_wire_parser_t parser;
    srr_wire_parser_init(&parser, rx_buf, sizeof(rx_buf), decode_buf, sizeof(decode_buf));
    srr_wire_header_t out_hdr;
    const uint8_t *out_pl = NULL;
    srr_parser_status_t st = SRR_PARSER_NEED_MORE;
    for (size_t i = 0; i < flen; i++) {
        st = srr_wire_parser_feed_byte(&parser, frame[i], &out_hdr, &out_pl);
    }
    assert(st == SRR_PARSER_FRAME_READY);
    assert(out_hdr.service_id == 42);
    assert(out_hdr.operation_id == 7);
    assert(out_hdr.session_id == 1001);
    assert(out_hdr.trans_id == 999999ULL);
    assert(out_hdr.payload_len == strlen(pl));
    assert(memcmp(out_pl, pl, strlen(pl)) == 0);

    printf("C reference implementation passes all canonical vector checks.\n");
    return 0;
}
