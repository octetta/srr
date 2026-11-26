#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include "srr/srr.h"

/*
 * ============================================================================
 * 1. In-Process Scatter-Gather Test
 * ============================================================================
 */
typedef struct {
    uint32_t cmd_id;
    uint32_t flags;
} msg_hdr_t;

typedef struct {
    int32_t  status;
    uint32_t item_count;
} reply_hdr_t;

static void *inproc_scatter_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    assert(srr_receive(chan, &req) == SRR_OK);
    assert(srr_request_size(req) == sizeof(msg_hdr_t) + 12);

    const msg_hdr_t *hdr = (const msg_hdr_t *)srr_request_data(req);
    assert(hdr->cmd_id == 0x1234);
    assert(hdr->flags == 0x0001);

    const char *payload_text = (const char *)srr_request_data(req) + sizeof(msg_hdr_t);
    assert(memcmp(payload_text, "HELLO_VECTOR", 12) == 0);

    /* Server replies using 2 scatter-gather vectors */
    reply_hdr_t rep_hdr = { .status = 0, .item_count = 42 };
    const char *rep_data = "REPLY_SCATTERED_OK";

    srr_iovec_t rep_iov[2] = {
        { .iov_base = &rep_hdr,           .iov_len = sizeof(rep_hdr) },
        { .iov_base = (void *)rep_data,   .iov_len = strlen(rep_data) + 1 }
    };

    assert(srr_replyv(req, rep_iov, 2) == SRR_OK);
    return NULL;
}

static void test_inproc_scatter_gather(void) {
    printf("[TEST] In-process scatter-gather send & reply... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, inproc_scatter_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    msg_hdr_t req_hdr = { .cmd_id = 0x1234, .flags = 0x0001 };
    const char *req_body = "HELLO_VECTOR";

    srr_iovec_t send_iov[2] = {
        { .iov_base = &req_hdr,         .iov_len = sizeof(req_hdr) },
        { .iov_base = (void *)req_body, .iov_len = 12 }
    };

    reply_hdr_t rep_hdr = {0};
    char rep_body[32] = {0};

    srr_iovec_t reply_iov[2] = {
        { .iov_base = &rep_hdr, .iov_len = sizeof(rep_hdr) },
        { .iov_base = rep_body, .iov_len = sizeof(rep_body) }
    };

    size_t out_rep_sz = 0;
    srr_status_t st = srr_sendv(conn, send_iov, 2, reply_iov, 2, &out_rep_sz);
    assert(st == SRR_OK);
    assert(out_rep_sz == sizeof(reply_hdr_t) + strlen("REPLY_SCATTERED_OK") + 1);
    assert(rep_hdr.status == 0);
    assert(rep_hdr.item_count == 42);
    assert(strcmp(rep_body, "REPLY_SCATTERED_OK") == 0);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * 2. Host OS IPC (Unix Domain Socket) Scatter-Gather Test
 * ============================================================================
 */
static void *ipc_scatter_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    assert(srr_receive(chan, &req) == SRR_OK);
    size_t sz = srr_request_size(req);
    assert(sz == 4 + 8 + 6);

    const char *data = (const char *)srr_request_data(req);
    assert(memcmp(data, "HEAD", 4) == 0);
    assert(memcmp(data + 4, "BODY_001", 8) == 0);
    assert(memcmp(data + 12, "TAIL_2", 6) == 0);

    /* Server replies with 2 vectors */
    const char *p1 = "ACK_";
    const char *p2 = "DATA_COMPLETE";
    srr_iovec_t rep_iov[2] = {
        { .iov_base = (void *)p1, .iov_len = 4 },
        { .iov_base = (void *)p2, .iov_len = 14 }
    };

    assert(srr_replyv(req, rep_iov, 2) == SRR_OK);
    return NULL;
}

static void test_ipc_scatter_gather(void) {
    printf("[TEST] Host IPC (Unix Socket) scatter-gather (writev/readv)... ");
    const char *svc = "test_scatter_ipc";
    srr_channel_t *chan = NULL;
    assert(srr_channel_create_service(svc, NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, ipc_scatter_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    srr_iovec_t send_iov[3] = {
        { .iov_base = (void *)"HEAD",     .iov_len = 4 },
        { .iov_base = (void *)"BODY_001", .iov_len = 8 },
        { .iov_base = (void *)"TAIL_2",   .iov_len = 6 }
    };

    char r1[5] = {0};
    char r2[15] = {0};
    srr_iovec_t rep_iov[2] = {
        { .iov_base = r1, .iov_len = 4 },
        { .iov_base = r2, .iov_len = 14 }
    };

    size_t out_sz = 0;
    srr_status_t st = srr_sendv(conn, send_iov, 3, rep_iov, 2, &out_sz);
    assert(st == SRR_OK);
    assert(out_sz == 18);
    assert(memcmp(r1, "ACK_", 4) == 0);
    assert(memcmp(r2, "DATA_COMPLETE", 14) == 0);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * 3. Truncation and Buffer Capacity Edge Cases
 * ============================================================================
 */
static void *trunc_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    assert(srr_receive(chan, &req) == SRR_OK);

    const char *msg = "1234567890ABCDEF";
    assert(srr_reply(req, msg, 16) == SRR_TOO_LARGE);
    return NULL;
}

static void test_scatter_truncation(void) {
    printf("[TEST] Scatter-gather capacity checking (SRR_TOO_LARGE)... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, trunc_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    char b1[4] = {0};
    char b2[4] = {0};
    srr_iovec_t rep_iov[2] = {
        { .iov_base = b1, .iov_len = 4 },
        { .iov_base = b2, .iov_len = 4 }
    }; /* Total capacity 8 bytes, server will send 16 */

    srr_iovec_t send_iov = { .iov_base = (void *)"PING", .iov_len = 4 };
    size_t out_sz = 0;
    srr_status_t st = srr_sendv(conn, &send_iov, 1, rep_iov, 2, &out_sz);
    assert(st == SRR_TOO_LARGE);
    assert(out_sz == 16);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

/*
 * ============================================================================
 * 4. Zero-Length Vector and Asymmetric Formats
 * ============================================================================
 */
static void *asymmetric_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    assert(srr_receive(chan, &req) == SRR_OK);
    assert(srr_request_size(req) == 8);

    /* Single contiguous reply */
    assert(srr_reply(req, "RESPONSE", 8) == SRR_OK);
    return NULL;
}

static void test_asymmetric_vectors(void) {
    printf("[TEST] Asymmetric vector combinations (empty slices, single/multi)... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, asymmetric_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    srr_iovec_t send_iov[3] = {
        { .iov_base = NULL,         .iov_len = 0 }, /* Empty leading vector */
        { .iov_base = (void *)"TEST", .iov_len = 4 },
        { .iov_base = (void *)"DATA", .iov_len = 4 }
    };

    char r1[4] = {0};
    char r2[4] = {0};
    srr_iovec_t rep_iov[2] = {
        { .iov_base = r1, .iov_len = 4 },
        { .iov_base = r2, .iov_len = 4 }
    };

    size_t out_sz = 0;
    srr_status_t st = srr_sendv(conn, send_iov, 3, rep_iov, 2, &out_sz);
    assert(st == SRR_OK);
    assert(out_sz == 8);
    assert(memcmp(r1, "RESP", 4) == 0);
    assert(memcmp(r2, "ONSE", 4) == 0);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Scatter-Gather I/O Unit Test Suite ===\n");
    test_inproc_scatter_gather();
    test_ipc_scatter_gather();
    test_scatter_truncation();
    test_asymmetric_vectors();
    printf("All Scatter-Gather I/O tests passed!\n");
    return 0;
}
