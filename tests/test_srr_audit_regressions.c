#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "srr/srr.h"
#include "srr/srr_embedded.h"
#include "srr/srr_tiny.h"
#include "../src/srr_internal.h"
#include "../src/srr_proto.h"

/*
 * ============================================================================
 * Regression Test 1: IPC Timed-Out Request Closes Connection
 * ============================================================================
 * When an IPC request times out, the stream state is indeterminate (server
 * may write later). The client connection must be marked disconnected so
 * subsequent requests do NOT read stale bytes.
 */
static void *slow_ipc_server_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    if (srr_receive(chan, &req) == SRR_OK) {
        usleep(50000); /* 50ms delay */
        srr_reply(req, "LATE_REPLY", 10);
    }
    return NULL;
}

void test_regression_ipc_timeout_disconnect(void) {
    printf("[REGRESSION 1] IPC timeout prevents stale reply delivery... ");
    const char *svc = "test_reg_ipc_to";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, slow_ipc_server_thread, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    char reply[64] = {0};
    size_t reply_sz = 0;
    /* Timeout in 10ms (server takes 50ms) */
    srr_status_t st = srr_send_timed(conn, "PING", 4, reply, sizeof(reply), &reply_sz, 10);
    assert(st == SRR_TIMEOUT);

    /* Wait for server to finish late reply */
    pthread_join(th, NULL);

    /* A subsequent request on the timed-out connection MUST fail with SRR_DISCONNECTED
     * rather than reading the stale "LATE_REPLY" bytes from the first request! */
    st = srr_send_timed(conn, "PING2", 5, reply, sizeof(reply), &reply_sz, 100);
    assert(st == SRR_DISCONNECTED);

    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 2: IPC Reply Transaction-ID Validation
 * ============================================================================
 */
static void *bogus_tid_server_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    if (srr_receive(chan, &req) == SRR_OK) {
        /* Tamper with ipc_trans_id to simulate misaligned / stale reply */
        req->ipc_trans_id = 99999;
        srr_reply(req, "BOGUS_TID", 9);
    }
    return NULL;
}

void test_regression_ipc_trans_id_validation(void) {
    printf("[REGRESSION 2] IPC mismatched transaction-ID rejection... ");
    const char *svc = "test_reg_tid";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, bogus_tid_server_thread, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    char reply[64] = {0};
    size_t reply_sz = 0;
    srr_status_t st = srr_send_timed(conn, "TEST", 4, reply, sizeof(reply), &reply_sz, 500);
    /* Client must detect transaction ID mismatch and return SRR_PROTOCOL_ERROR */
    assert(st == SRR_PROTOCOL_ERROR);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 3: IPC Reply and Error Paths Detect Transport Write Failure
 * ============================================================================
 */
void test_regression_ipc_write_failure_detection(void) {
    printf("[REGRESSION 3] IPC reply detects transport write failure... ");
    const char *svc = "test_reg_write_fail";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    /* Send request in non-blocking fashion using raw socket to close immediately */
    int raw_fd = (int)conn->ipc_handle;
    srr_frame_header_t req_hdr = {
        .magic = SRR_PROTO_MAGIC,
        .version = SRR_PROTO_VERSION,
        .msg_type = SRR_MSG_REQUEST,
        .flags = 0,
        .trans_id = 1,
        .payload_len = 4
    };
    assert(write(raw_fd, &req_hdr, sizeof(req_hdr)) == (ssize_t)sizeof(req_hdr));
    assert(write(raw_fd, "TEST", 4) == 4);

    /* Sever the client connection immediately */
    close(raw_fd);
    conn->ipc_handle = 0;
    conn->connected = false;

    srr_request_t *req = NULL;
    srr_status_t rst = srr_receive(chan, &req);
    assert(rst == SRR_OK);
    assert(req != NULL);

    /* Server attempts to reply to disconnected client */
    srr_status_t reply_st = srr_reply(req, "ACK", 3);
    /* Server MUST receive SRR_DISCONNECTED instead of false SRR_OK */
    assert(reply_st == SRR_DISCONNECTED);

    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 4: IPC Request Disposal Unblocks Client with SRR_CANCELLED
 * ============================================================================
 */
static void *abandoning_server_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    if (srr_receive(chan, &req) == SRR_OK) {
        /* Drop without reply */
        srr_request_dispose(req);
    }
    return NULL;
}

void test_regression_ipc_request_dispose_cancels(void) {
    printf("[REGRESSION 4] IPC srr_request_dispose unblocks client with SRR_CANCELLED... ");
    const char *svc = "test_reg_dispose";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, abandoning_server_thread, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    char reply[64] = {0};
    size_t reply_sz = 0;
    srr_status_t st = srr_send(conn, "ABANDON_ME", 10, reply, sizeof(reply), &reply_sz);
    /* Client MUST unblock immediately with SRR_CANCELLED */
    assert(st == SRR_CANCELLED);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 5: IPC Transaction State Arbitration (Single Winner)
 * ============================================================================
 */
void test_regression_ipc_single_winner_arbitration(void) {
    printf("[REGRESSION 5] IPC single-winner reply state arbitration... ");
    const char *svc = "test_reg_arbitrate";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    int raw_fd = (int)conn->ipc_handle;
    srr_frame_header_t req_hdr = {
        .magic = SRR_PROTO_MAGIC,
        .version = SRR_PROTO_VERSION,
        .msg_type = SRR_MSG_REQUEST,
        .flags = 0,
        .trans_id = 42,
        .payload_len = 4
    };
    assert(write(raw_fd, &req_hdr, sizeof(req_hdr)) == (ssize_t)sizeof(req_hdr));
    assert(write(raw_fd, "PING", 4) == 4);

    srr_request_t *req = NULL;
    assert(srr_receive(chan, &req) == SRR_OK);

    /* Retain request so we can safely test second reply attempt */
    srr_request_retain(req);

    /* First reply: Winner */
    srr_status_t st1 = srr_reply(req, "WINNER", 6);
    assert(st1 == SRR_OK);

    /* Second reply on same request: Must be strictly rejected with SRR_INVALID */
    srr_status_t st2 = srr_reply(req, "LOSER", 5);
    assert(st2 == SRR_INVALID); /* Repeated reply strictly rejected without double-write */

    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 6: Embedded Service Handler Reply Length Bounds
 * ============================================================================
 */
static int buggy_oversized_handler(
    uint16_t service_id, uint16_t op_id,
    const uint8_t *req, size_t req_len,
    uint8_t *reply_buf, size_t reply_cap,
    size_t *actual_reply_len, void *user_data)
{
    (void)service_id; (void)op_id; (void)req; (void)req_len; (void)reply_buf; (void)user_data;
    /* Intentionally claim reply is larger than allocated reply_cap */
    *actual_reply_len = reply_cap + 100;
    return 0;
}

static uint8_t s_mock_tx[256];
static size_t  s_mock_tx_len = 0;
static int mock_tx_capture(const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    memcpy(s_mock_tx, data, len);
    s_mock_tx_len = len;
    return 0;
}

void test_regression_embedded_reply_bounds(void) {
    printf("[REGRESSION 6] Embedded reply length exceeding server reply capacity... ");
    srr_endpoint_t ep;
    uint8_t rx_buf[128], dec_buf[128], tx_buf[128], rep_buf[32];

    assert(srr_endpoint_init(
        &ep, 1, mock_tx_capture, NULL,
        rx_buf, sizeof(rx_buf),
        dec_buf, sizeof(dec_buf),
        tx_buf, sizeof(tx_buf),
        rep_buf, sizeof(rep_buf)
    ) == 0);

    assert(srr_endpoint_register_service(&ep, 10, buggy_oversized_handler, NULL) == 0);

    /* Feed an incoming request */
    srr_wire_header_t hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .flags = 0,
        .service_id = 10,
        .operation_id = 1,
        .reserved = 0,
        .session_id = 1,
        .trans_id = 55,
        .payload_len = 0
    };
    uint8_t frame[64];
    size_t enc = srr_wire_encode_frame(&hdr, NULL, 0, frame, sizeof(frame));
    assert(enc > 0);

    srr_endpoint_feed_bytes(&ep, frame, enc);

    /* Verify that an ERROR frame was returned instead of buffer over-read */
    srr_wire_header_t out_hdr;
    const uint8_t *out_pl = NULL;
    srr_wire_parser_t parser;
    uint8_t p_rx[128], p_dec[128];
    srr_wire_parser_init(&parser, p_rx, sizeof(p_rx), p_dec, sizeof(p_dec));

    srr_parser_status_t st = SRR_PARSER_NEED_MORE;
    for (size_t i = 0; i < s_mock_tx_len; i++) {
        st = srr_wire_parser_feed_byte(&parser, s_mock_tx[i], &out_hdr, &out_pl);
        if (st == SRR_PARSER_FRAME_READY) break;
    }
    assert(st == SRR_PARSER_FRAME_READY);
    assert(out_hdr.msg_type == SRR_WIRE_ERROR);
    assert(out_hdr.flags == 7); /* SRR_TOO_LARGE */
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 7: Tiny Profile Rejecting Oversized Requests
 * ============================================================================
 */
static int tiny_mock_handler(
    uint16_t service_id, uint16_t op_id,
    const uint8_t *req, uint8_t req_len,
    uint8_t *reply_buf, uint8_t reply_cap,
    uint8_t *actual_reply_len, void *user_data)
{
    (void)service_id; (void)op_id; (void)req; (void)req_len; (void)reply_buf; (void)reply_cap; (void)user_data;
    *actual_reply_len = 0;
    return 0;
}

void test_regression_tiny_oversized_request(void) {
    printf("[REGRESSION 7] Tiny profile rejects oversized requests with SRR_WIRE_ERROR... ");
    srr_tiny_endpoint_t ep;
    srr_tiny_init(&ep, 1, mock_tx_capture, NULL);
    assert(srr_tiny_register_service(&ep, 5, tiny_mock_handler, NULL) == 0);

    /* Create an oversized request exceeding SRR_TINY_MAX_PAYLOAD (32 bytes) */
    uint8_t large_payload[48];
    memset(large_payload, 0xAB, sizeof(large_payload));

    srr_wire_header_t hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .flags = 0,
        .service_id = 5,
        .operation_id = 1,
        .reserved = 0,
        .session_id = 1,
        .trans_id = 77,
        .payload_len = sizeof(large_payload)
    };
    uint8_t frame[128];
    size_t enc = srr_wire_encode_frame(&hdr, large_payload, sizeof(large_payload), frame, sizeof(frame));
    assert(enc > 0);

    for (size_t i = 0; i < enc; i++) {
        srr_tiny_feed_byte(&ep, frame[i]);
    }

    /* Verify that Tiny replied with SRR_WIRE_ERROR (not silent truncation!) */
    srr_wire_header_t out_hdr;
    const uint8_t *out_pl = NULL;
    srr_wire_parser_t parser;
    uint8_t p_rx[128], p_dec[128];
    srr_wire_parser_init(&parser, p_rx, sizeof(p_rx), p_dec, sizeof(p_dec));

    srr_parser_status_t st = SRR_PARSER_NEED_MORE;
    for (size_t i = 0; i < s_mock_tx_len; i++) {
        st = srr_wire_parser_feed_byte(&parser, s_mock_tx[i], &out_hdr, &out_pl);
        if (st == SRR_PARSER_FRAME_READY) break;
    }
    assert(st == SRR_PARSER_FRAME_READY);
    assert(out_hdr.msg_type == SRR_WIRE_ERROR);
    assert(out_hdr.flags == 7); /* SRR_TOO_LARGE */
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 8: Embedded Timer Wraparound Correctness
 * ============================================================================
 */
void test_regression_embedded_timer_wraparound(void) {
    printf("[REGRESSION 8] Embedded timer wraparound across 0xFFFFFFFF boundary... ");
    srr_endpoint_t ep;
    uint8_t rx_buf[128], dec_buf[128], tx_buf[128], rep_buf[32];
    assert(srr_endpoint_init(
        &ep, 1, mock_tx_capture, NULL,
        rx_buf, sizeof(rx_buf),
        dec_buf, sizeof(dec_buf),
        tx_buf, sizeof(tx_buf),
        rep_buf, sizeof(rep_buf)
    ) == 0);

    /* Begin request at near-wraparound timestamp (e.g. 50ms before UINT32_MAX) */
    uint32_t now_ms = 0xFFFFFFCD; /* UINT32_MAX - 50 */
    uint32_t timeout_ms = 100;    /* Target deadline wraps to 49 */
    uint8_t reply_buf[16];

    uint64_t tid = srr_begin_request(&ep, 1, 1, "REQ", 3, reply_buf, sizeof(reply_buf), timeout_ms, now_ms);
    assert(tid > 0);

    /* Advance time across wraparound boundary to 20ms past start (now = 0xFFFFFFE1) */
    now_ms += 20;
    srr_endpoint_tick(&ep, now_ms);
    /* MUST NOT be timed out yet! */
    assert(ep.transactions[0].state == SRR_TRANS_PENDING);

    /* Advance time to 10ms after wraparound (now = 10, total elapsed = 60ms) */
    now_ms = 10;
    srr_endpoint_tick(&ep, now_ms);
    /* MUST NOT be timed out yet! (100ms timeout has not expired) */
    assert(ep.transactions[0].state == SRR_TRANS_PENDING);

    /* Advance time to 101ms elapsed (now = 51) */
    now_ms = 51;
    srr_endpoint_tick(&ep, now_ms);
    /* NOW it must be timed out! */
    assert(ep.transactions[0].state == SRR_TRANS_TIMED_OUT);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 9: Wire Encode Rejection of NULL Payload with Nonzero Length
 * ============================================================================
 */
void test_regression_wire_encode_null_payload_rejection(void) {
    printf("[REGRESSION 9] srr_wire_encode_frame rejects NULL payload with nonzero length... ");
    srr_wire_header_t hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .service_id = 1,
        .trans_id = 100
    };
    uint8_t out[128];
    /* Nonzero length with NULL payload MUST return 0 (rejection) */
    size_t sz = srr_wire_encode_frame(&hdr, NULL, 50, out, sizeof(out));
    assert(sz == 0);

    /* Zero length with NULL payload is valid */
    sz = srr_wire_encode_frame(&hdr, NULL, 0, out, sizeof(out));
    assert(sz > 0);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 10: srr_pulse Reports Broken IPC Write Failures
 * ============================================================================
 */
void test_regression_pulse_ipc_write_failure(void) {
    printf("[REGRESSION 10] srr_pulse reports IPC write failures as SRR_DISCONNECTED... ");
    const char *svc = "test_reg_pulse_fail";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    /* Destroy server channel so client socket becomes severed */
    srr_channel_destroy(chan);

    /* srr_pulse must detect the broken socket and return SRR_DISCONNECTED */
    srr_status_t st = srr_pulse(conn, 1, 100);
    assert(st == SRR_DISCONNECTED);

    srr_disconnect(conn);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 11: srr_reply Validates Pointer/Length & Rejects Repetition
 * ============================================================================
 */
struct reply_val_ctx {
    srr_connection_t *conn;
    srr_status_t status;
};

static void *reply_val_client_th_fn(void *arg) {
    struct reply_val_ctx *ctx = (struct reply_val_ctx *)arg;
    char rep[64];
    size_t rlen = 0;
    ctx->status = srr_send(ctx->conn, "HELLO", 5, rep, sizeof(rep), &rlen);
    return NULL;
}

void test_regression_reply_validation_and_idempotency(void) {
    printf("[REGRESSION 11] srr_reply validates pointers and strictly rejects repeated replies... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    /* In a thread: send request */
    pthread_t th;
    struct reply_val_ctx ctx = { conn, SRR_OK };

    pthread_create(&th, NULL, reply_val_client_th_fn, &ctx);

    srr_request_t *req = NULL;
    assert(srr_receive(chan, &req) == SRR_OK);

    /* 1. Invalid pointer with nonzero length MUST fail with SRR_INVALID without mutating state */
    srr_status_t st = srr_reply(req, NULL, 32);
    assert(st == SRR_INVALID);

    /* Retain request so caller holds an extra reference to safely test repeated reply rejection */
    srr_request_retain(req);

    /* 2. Valid reply succeeds */
    st = srr_reply(req, "OK", 3);
    assert(st == SRR_OK);

    /* 3. A repeated reply attempt on the same completed request must fail with SRR_INVALID */
    st = srr_reply(req, "AGAIN", 6);
    assert(st == SRR_INVALID);

    pthread_join(th, NULL);
    assert(ctx.status == SRR_OK);

    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 12: Embedded Pending Registration, Rollback, and Capacity
 * ============================================================================
 */
static int failing_tx(const uint8_t *data, size_t len, void *user_data) {
    (void)data; (void)len; (void)user_data;
    return -1; /* Simulated hardware transmission failure */
}

void test_regression_embedded_tx_lifecycle(void) {
    printf("[REGRESSION 12] Embedded transaction registration, rollback, and capacity... ");
    srr_endpoint_t ep;
    uint8_t rx_buf[128], dec_buf[128], tx_buf[128], rep_buf[32];
    assert(srr_endpoint_init(
        &ep, 1, mock_tx_capture, NULL,
        rx_buf, sizeof(rx_buf),
        dec_buf, sizeof(dec_buf),
        tx_buf, sizeof(tx_buf),
        rep_buf, sizeof(rep_buf)
    ) == 0);

    /* 1. Test rollback on transmit failure */
    ep.tx_fn = failing_tx;
    uint8_t c_rep[32];
    uint64_t tid = srr_begin_request(&ep, 1, 1, "REQ", 3, c_rep, sizeof(c_rep), 100, 1000);
    assert(tid == 0);
    assert(ep.transactions[0].state == SRR_TRANS_UNUSED); /* Rolled back */

    /* 2. Test successful registration */
    ep.tx_fn = mock_tx_capture;
    tid = srr_begin_request(&ep, 1, 1, "REQ", 3, c_rep, 4 /* small capacity = 4 */, 100, 1000);
    assert(tid > 0);
    assert(ep.transactions[0].state == SRR_TRANS_PENDING);

    /* 3. Feed reply that overflows caller's reply_cap (10 bytes > 4 bytes) */
    srr_wire_header_t r_hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REPLY,
        .session_id = 1,
        .trans_id = tid,
        .service_id = 1,
        .operation_id = 1,
        .payload_len = 0
    };
    uint8_t frame[128];
    const char *large_reply = "OVERFLOW_PAYLOAD";
    size_t flen = srr_wire_encode_frame(&r_hdr, large_reply, strlen(large_reply), frame, sizeof(frame));
    assert(flen > 0);

    for (size_t i = 0; i < flen; i++) {
        srr_endpoint_feed_byte(&ep, frame[i]);
    }

    /* Overflow MUST transition state to SRR_TRANS_ERROR with error_code 7 (SRR_TOO_LARGE) */
    assert(ep.transactions[0].state == SRR_TRANS_ERROR);
    assert(ep.transactions[0].error_code == 7);

    /* Collect reply and verify slot is cleared for reuse */
    size_t out_len = 0;
    int err = 0;
    int rc = srr_collect_reply(&ep, tid, &out_len, &err);
    assert(rc == -1);
    assert(err == 7);
    assert(ep.transactions[0].state == SRR_TRANS_UNUSED);

    /* 4. Test cancellation and slot reuse */
    tid = srr_begin_request(&ep, 1, 1, "REQ2", 4, c_rep, sizeof(c_rep), 100, 1000);
    assert(tid > 0);
    assert(srr_cancel_transaction(&ep, tid) == 0);
    assert(srr_poll_transaction(&ep, tid) == SRR_TRANS_UNUSED); /* Slot immediately freed for reuse */

    printf("PASS\n");
}

/*
 * ============================================================================
 * Regression Test 13: Concurrent Channel Close & IPC Server Stop Idempotency
 * ============================================================================
 */
static void *concurrent_close_worker(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    for (int i = 0; i < 5; i++) {
        srr_channel_close(chan);
        usleep(1000);
    }
    return NULL;
}

void test_regression_concurrent_close_idempotency(void) {
    printf("[REGRESSION 13] Concurrent channel close & IPC shutdown idempotency... ");
    const char *svc = "test_reg_conc_close";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, concurrent_close_worker, chan);
    pthread_create(&t2, NULL, concurrent_close_worker, chan);
    pthread_create(&t3, NULL, concurrent_close_worker, chan);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    srr_channel_destroy(chan);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Correctness & Regression Audit Test Suite ===\n");
    test_regression_ipc_timeout_disconnect();
    test_regression_ipc_trans_id_validation();
    test_regression_ipc_write_failure_detection();
    test_regression_ipc_request_dispose_cancels();
    test_regression_ipc_single_winner_arbitration();
    test_regression_embedded_reply_bounds();
    test_regression_tiny_oversized_request();
    test_regression_embedded_timer_wraparound();
    test_regression_wire_encode_null_payload_rejection();
    test_regression_pulse_ipc_write_failure();
    test_regression_reply_validation_and_idempotency();
    test_regression_embedded_tx_lifecycle();
    test_regression_concurrent_close_idempotency();
    printf("All 13 regression tests passed with 100%% precision!\n");
    return 0;
}
