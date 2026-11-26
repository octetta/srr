#include "srr/srr_sid.h"
#include "srr/srr_p1_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_sid_protocol_operations(void) {
    printf("Testing SRR SID Protocol operations against P1 Hub RAM engine...\n");

    srr_p1_hub_t hub;
    srr_p1_hub_init(&hub, 0x53494431, 0);

    uint8_t sid_regs[SRR_SID_TOTAL_REGS];
    memset(sid_regs, 0, sizeof(sid_regs));

    /* 1. Test OP_WRITE_REG: Set Voice 1 Freq Hi = 0x12 */
    srr_sid_reg_write_t wreg = { .reg = SRR_SID_V1_FREQ_HI, .val = 0x12 };
    srr_wire_header_t hdr1 = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x53494431,
        .trans_id = 101,
        .service_id = SRR_SVC_SID,
        .operation_id = SRR_SID_OP_WRITE_REG,
        .payload_len = sizeof(wreg)
    };
    uint8_t frame[128];
    size_t flen = srr_wire_encode_frame(&hdr1, &wreg, sizeof(wreg), frame, sizeof(frame));
    assert(flen > 0);

    /* Push frame into Hub RX ring */
    for (size_t i = 0; i < flen; i++) {
        hub.rx_ring[hub.rx_head] = frame[i];
        hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
    }

    /* Step Cog 0: Parse frame into mailbox */
    srr_p1_io_cog_step(&hub);
    assert(hub.mailbox.state == SRR_P1_MBOX_REQUEST_READY);
    assert(hub.mailbox.service_id == SRR_SVC_SID);
    assert(hub.mailbox.operation_id == SRR_SID_OP_WRITE_REG);

    /* Step Cog 1: Worker claims request */
    uint16_t op = 0;
    uint64_t tid = 0;
    uint8_t payload[64];
    uint8_t req_len = 0;
    bool claimed = srr_p1_worker_claim(&hub, 1, SRR_SVC_SID, &op, &tid, payload, &req_len);
    assert(claimed);
    assert(op == SRR_SID_OP_WRITE_REG);
    assert(tid == 101);
    assert(req_len == 2);
    assert(payload[0] == SRR_SID_V1_FREQ_HI);
    assert(payload[1] == 0x12);

    /* Apply to simulated SID registers */
    sid_regs[payload[0]] = payload[1];
    assert(sid_regs[SRR_SID_V1_FREQ_HI] == 0x12);

    /* Reply */
    uint8_t status = 0;
    int rc = srr_p1_worker_reply(&hub, 1, tid, &status, 1, false);
    assert(rc == 0);
    assert(hub.mailbox.state == SRR_P1_MBOX_REPLY_READY);

    srr_p1_io_cog_step(&hub);
    assert(hub.mailbox.state == SRR_P1_MBOX_EMPTY);

    /* Drain TX ring */
    while (hub.tx_tail != hub.tx_head) {
        hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;
    }

    /* 2. Test OP_WRITE_BATCH */
    srr_sid_reg_write_t batch[3] = {
        { .reg = SRR_SID_V1_CR, .val = SRR_SID_CR_SAW | SRR_SID_CR_GATE },
        { .reg = SRR_SID_V1_AD, .val = 0x24 },
        { .reg = SRR_SID_MODE_VOL, .val = 0x0F }
    };
    srr_wire_header_t hdr2 = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x53494431,
        .trans_id = 102,
        .service_id = SRR_SVC_SID,
        .operation_id = SRR_SID_OP_WRITE_BATCH,
        .payload_len = sizeof(batch)
    };
    flen = srr_wire_encode_frame(&hdr2, batch, sizeof(batch), frame, sizeof(frame));
    for (size_t i = 0; i < flen; i++) {
        hub.rx_ring[hub.rx_head] = frame[i];
        hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
    }
    srr_p1_io_cog_step(&hub);
    claimed = srr_p1_worker_claim(&hub, 1, SRR_SVC_SID, &op, &tid, payload, &req_len);
    assert(claimed);
    assert(op == SRR_SID_OP_WRITE_BATCH);
    assert(req_len == 6);
    for (int i = 0; i + 1 < req_len; i += 2) {
        sid_regs[payload[i]] = payload[i + 1];
    }
    assert(sid_regs[SRR_SID_V1_CR] == (SRR_SID_CR_SAW | SRR_SID_CR_GATE));
    assert(sid_regs[SRR_SID_V1_AD] == 0x24);
    assert(sid_regs[SRR_SID_MODE_VOL] == 0x0F);

    uint8_t count = 3;
    srr_p1_worker_reply(&hub, 1, tid, &count, 1, false);
    srr_p1_io_cog_step(&hub);
    while (hub.tx_tail != hub.tx_head) hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;

    /* 3. Test Multi-Chip Dispatch: Target Chip 1 (0x0051) and Chip 2 (0x0052) */
    uint8_t sid_chip1_regs[SRR_SID_TOTAL_REGS] = {0};
    srr_sid_reg_write_t wreg1 = { .reg = SRR_SID_V1_FREQ_LO, .val = 0xAA };
    srr_wire_header_t hdr_chip1 = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x53494431,
        .trans_id = 103,
        .service_id = SRR_SVC_SID_CHIP(1), /* SID #1 */
        .operation_id = SRR_SID_OP_WRITE_REG,
        .payload_len = sizeof(wreg1)
    };
    flen = srr_wire_encode_frame(&hdr_chip1, &wreg1, sizeof(wreg1), frame, sizeof(frame));
    for (size_t i = 0; i < flen; i++) {
        hub.rx_ring[hub.rx_head] = frame[i];
        hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
    }
    srr_p1_io_cog_step(&hub);
    assert(hub.mailbox.service_id == SRR_SVC_SID_CHIP(1));
    claimed = srr_p1_worker_claim(&hub, 3, SRR_SVC_SID_CHIP(1), &op, &tid, payload, &req_len);
    assert(claimed);
    assert(op == SRR_SID_OP_WRITE_REG);
    assert(req_len == 2);
    sid_chip1_regs[payload[0]] = payload[1];
    assert(sid_chip1_regs[SRR_SID_V1_FREQ_LO] == 0xAA);
    assert(sid_regs[SRR_SID_V1_FREQ_LO] == 0x00); /* Chip 0 untouched */

    status = 0;
    srr_p1_worker_reply(&hub, 3, tid, &status, 1, false);
    srr_p1_io_cog_step(&hub);
    while (hub.tx_tail != hub.tx_head) hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;

    /* 4. Test Multi-Chip Batch Write Tuple: [{chip, reg, val}] */
    srr_sid_multi_reg_t multi_batch[2] = {
        { .chip = 0, .reg = SRR_SID_MODE_VOL, .val = 0x0E },
        { .chip = 1, .reg = SRR_SID_MODE_VOL, .val = 0x0F }
    };
    srr_wire_header_t hdr_multi = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x53494431,
        .trans_id = 104,
        .service_id = SRR_SVC_SID_BASE,
        .operation_id = SRR_SID_OP_MULTI_WRITE_BATCH,
        .payload_len = sizeof(multi_batch)
    };
    flen = srr_wire_encode_frame(&hdr_multi, multi_batch, sizeof(multi_batch), frame, sizeof(frame));
    for (size_t i = 0; i < flen; i++) {
        hub.rx_ring[hub.rx_head] = frame[i];
        hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
    }
    srr_p1_io_cog_step(&hub);
    claimed = srr_p1_worker_claim(&hub, 2, SRR_SVC_SID_BASE, &op, &tid, payload, &req_len);
    assert(claimed);
    assert(op == SRR_SID_OP_MULTI_WRITE_BATCH);
    assert(req_len == sizeof(multi_batch));

    status = 2;
    srr_p1_worker_reply(&hub, 2, tid, &status, 1, false);
    srr_p1_io_cog_step(&hub);
    while (hub.tx_tail != hub.tx_head) hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;

    /* 5. Test Frequency and Note Helpers */
    uint16_t freq_440 = srr_sid_freq_hz(440.0f);
    assert(freq_440 == 7492);

    uint16_t midi_a4 = srr_sid_freq_midi(69); /* Note 69 = A4 */
    assert(midi_a4 == 7558);

    uint16_t midi_c4 = srr_sid_freq_midi(60); /* Middle C = C4 */
    assert(midi_c4 == 4494);

    printf("SRR SID Protocol operations test PASSED.\n");
}

int main(void) {
    test_sid_protocol_operations();
    return 0;
}
