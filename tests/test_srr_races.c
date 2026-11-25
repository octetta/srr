#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "srr/srr.h"

static void *slow_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    if (srr_receive(chan, &req) == SRR_OK) {
        /* Sleep 50ms before replying */
        struct timespec ts = {0, 50 * 1000000L};
        nanosleep(&ts, NULL);
        const char *rep = "late_reply";
        srr_status_t rc = srr_reply(req, rep, strlen(rep) + 1);
        /* Server should see SRR_TIMEOUT since client timed out at 10ms */
        assert(rc == SRR_TIMEOUT);
    }
    return NULL;
}

void test_timeout_race(void) {
    printf("[TEST] Timeout race vs late reply... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, slow_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    char reply[64] = {0};
    size_t reply_sz = 0;
    /* Timeout in 10ms */
    srr_status_t st = srr_send_timed(conn, "ping", 5, reply, sizeof(reply), &reply_sz, 10);
    assert(st == SRR_TIMEOUT);

    pthread_join(th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

static void *blocked_receiver(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;
    srr_status_t st = srr_receive(chan, &req);
    assert(st == SRR_CLOSED);
    return NULL;
}

void test_channel_close_unblocks_receiver(void) {
    printf("[TEST] Channel close wakes blocked receiver... ");
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(NULL, &chan) == SRR_OK);

    pthread_t th;
    pthread_create(&th, NULL, blocked_receiver, chan);

    /* Brief sleep to guarantee receiver is blocked inside srr_receive */
    struct timespec ts = {0, 10 * 1000000L};
    nanosleep(&ts, NULL);

    assert(srr_channel_close(chan) == SRR_OK);
    pthread_join(th, NULL);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Race Tests ===\n");
    test_timeout_race();
    test_channel_close_unblocks_receiver();
    printf("All race tests passed!\n");
    return 0;
}
