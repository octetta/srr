#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "srr/srr.h"

#define NUM_CLIENTS 8
#define NUM_SERVERS 4
#define REQS_PER_CLIENT 500

typedef struct {
    srr_channel_t *chan;
    int            server_id;
} server_ctx_t;

typedef struct {
    srr_channel_t *chan;
    int            client_id;
} client_ctx_t;

static void *stress_server_worker(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(ctx->chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            srr_request_dispose(req);
            continue;
        }

        uint64_t val = 0;
        assert(srr_request_size(req) == sizeof(uint64_t));
        memcpy(&val, srr_request_data(req), sizeof(uint64_t));

        uint64_t reply_val = val + 1;
        srr_status_t rc = srr_reply(req, &reply_val, sizeof(reply_val));
        assert(rc == SRR_OK);
    }
    return NULL;
}

static void *stress_client_worker(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(ctx->chan, NULL, &conn) == SRR_OK);

    for (int i = 0; i < REQS_PER_CLIENT; i++) {
        uint64_t req_val = ((uint64_t)ctx->client_id << 32) | (uint64_t)i;
        uint64_t reply_val = 0;
        size_t reply_sz = 0;

        srr_status_t st = srr_send(
            conn,
            &req_val,
            sizeof(req_val),
            &reply_val,
            sizeof(reply_val),
            &reply_sz);

        assert(st == SRR_OK);
        assert(reply_sz == sizeof(uint64_t));
        assert(reply_val == req_val + 1);
    }

    srr_disconnect(conn);
    return NULL;
}

void test_stress_concurrency(void) {
    printf("[TEST] Concurrency stress (%d clients x %d requests, %d servers)... ",
           NUM_CLIENTS, REQS_PER_CLIENT, NUM_SERVERS);

    srr_channel_options_t opts = {
        .queue_capacity = 128,
        .max_request_size = 1024,
        .max_reply_size = 1024
    };
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    pthread_t servers[NUM_SERVERS];
    server_ctx_t server_ctxs[NUM_SERVERS];
    for (int i = 0; i < NUM_SERVERS; i++) {
        server_ctxs[i].chan = chan;
        server_ctxs[i].server_id = i;
        pthread_create(&servers[i], NULL, stress_server_worker, &server_ctxs[i]);
    }

    pthread_t clients[NUM_CLIENTS];
    client_ctx_t client_ctxs[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_ctxs[i].chan = chan;
        client_ctxs[i].client_id = i + 1;
        pthread_create(&clients[i], NULL, stress_client_worker, &client_ctxs[i]);
    }

    /* Wait for all clients to finish */
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(clients[i], NULL);
    }

    /* Close channel to wake servers */
    assert(srr_channel_close(chan) == SRR_OK);

    for (int i = 0; i < NUM_SERVERS; i++) {
        pthread_join(servers[i], NULL);
    }

    srr_channel_destroy(chan);
    printf("PASS\n");
}

int main(void) {
    printf("=== SRR Concurrency Stress Tests ===\n");
    test_stress_concurrency();
    printf("All stress tests passed!\n");
    return 0;
}
