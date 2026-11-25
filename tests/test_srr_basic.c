#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "srr/srr.h"

static void *server_echo_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            assert(srr_pulse_code(req) == 42);
            assert(srr_pulse_value(req) == 1001);
            srr_request_dispose(req);
            break; /* done */
        }

        const char *data = (const char *)srr_request_data(req);
        size_t size = srr_request_size(req);
        assert(size > 0);

        if (strcmp(data, "error_test") == 0) {
            srr_status_t rc = srr_reply_error(req, SRR_INVALID);
            assert(rc == SRR_OK);
        } else if (strcmp(data, "too_large_test") == 0) {
            char big_reply[128] = "this reply is too large for caller buffer";
            srr_status_t rc = srr_reply(req, big_reply, strlen(big_reply));
            assert(rc == SRR_TOO_LARGE);
        } else if (strcmp(data, "abandon_test") == 0) {
            /* Server drops it without reply */
            srr_request_dispose(req);
        } else {
            /* Echo with prefix */
            char reply[256];
            snprintf(reply, sizeof(reply), "ECHO:%s", data);
            srr_status_t rc = srr_reply(req, reply, strlen(reply) + 1);
            assert(rc == SRR_OK);
        }
    }
    return NULL;
}

void test_basic_exchange(void) {
    printf("[TEST] Basic send/receive/reply... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, server_echo_thread, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    /* 1. Normal Send/Reply */
    const char *msg = "hello srr";
    char reply[64] = {0};
    size_t reply_sz = 0;
    srr_status_t status = srr_send(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz);
    assert(status == SRR_OK);
    assert(strcmp(reply, "ECHO:hello srr") == 0);
    assert(reply_sz == strlen(reply) + 1);

    /* 2. Server Error Reply */
    const char *err_msg = "error_test";
    status = srr_send(conn, err_msg, strlen(err_msg) + 1, reply, sizeof(reply), &reply_sz);
    assert(status == SRR_INVALID);

    /* 3. Too Large Reply */
    const char *large_msg = "too_large_test";
    char small_buf[5] = {0};
    status = srr_send(conn, large_msg, strlen(large_msg) + 1, small_buf, sizeof(small_buf), &reply_sz);
    assert(status == SRR_TOO_LARGE);
    assert(reply_sz > sizeof(small_buf));

    /* 4. Abandoned Request */
    const char *ab_msg = "abandon_test";
    status = srr_send(conn, ab_msg, strlen(ab_msg) + 1, reply, sizeof(reply), &reply_sz);
    assert(status == SRR_CANCELLED);

    /* 5. Pulse */
    assert(srr_pulse(conn, 42, 1001) == SRR_OK);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Basic Tests ===\n");
    test_basic_exchange();
    printf("All basic tests passed!\n");
    return 0;
}
