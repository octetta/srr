#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "srr_udp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#define TEST_UDP_PORT 19876
#define SVC_UDP_ECHO  1
#define OP_ECHO       10

typedef struct {
    srr_udp_endpoint_t *srv_ep;
    pthread_mutex_t lock;
    bool running;
} udp_srv_ctx_t;

static void* udp_server_thread(void *arg) {
    udp_srv_ctx_t *ctx = (udp_srv_ctx_t *)arg;
    uint8_t payload[256];

    while (1) {
        pthread_mutex_lock(&ctx->lock);
        bool run = ctx->running;
        pthread_mutex_unlock(&ctx->lock);
        if (!run) break;

        srr_wire_header_t hdr;
        size_t actual_len = 0;
        srr_udp_peer_t *peer = NULL;

        int rc = srr_udp_server_receive(ctx->srv_ep, &hdr, payload, sizeof(payload), &actual_len, &peer, 200);
        if (rc == 0 && peer) {
            if (hdr.msg_type == SRR_WIRE_REQUEST && hdr.service_id == SVC_UDP_ECHO && hdr.operation_id == OP_ECHO) {
                /* Echo reply back to sender */
                srr_udp_server_reply(ctx->srv_ep, peer, hdr.trans_id, hdr.service_id, hdr.operation_id, payload, actual_len, false);
            } else if (hdr.msg_type == SRR_WIRE_PULSE) {
                /* Pulse handled without reply */
            }
            srr_udp_peer_free(peer);
        }
    }
    return NULL;
}

static void test_udp_roundtrip_and_pulse(void) {
    printf("Testing UDP transport request/reply round-trip and pulses...\n");

    srr_udp_config_t srv_cfg;
    srr_udp_default_config(&srv_cfg);
    srr_udp_endpoint_t *srv = srr_udp_server_create(TEST_UDP_PORT, &srv_cfg);
    assert(srv != NULL);

    udp_srv_ctx_t srv_ctx = { .srv_ep = srv, .running = true };
    pthread_mutex_init(&srv_ctx.lock, NULL);
    pthread_t th;
    pthread_create(&th, NULL, udp_server_thread, &srv_ctx);

    usleep(20000); /* 20ms */

    srr_udp_config_t cli_cfg;
    srr_udp_default_config(&cli_cfg);
    srr_udp_endpoint_t *cli = srr_udp_client_create("127.0.0.1", TEST_UDP_PORT, &cli_cfg);
    assert(cli != NULL);

    /* 1. Synchronous Request / Reply */
    const char *msg = "Hello UDP SRR!";
    char reply[128] = {0};
    size_t reply_len = 0;

    int rc = srr_udp_request(cli, SVC_UDP_ECHO, OP_ECHO, msg, strlen(msg), reply, sizeof(reply) - 1, &reply_len, 2000);
    assert(rc == 0);
    assert(reply_len == strlen(msg));
    assert(strcmp(reply, msg) == 0);
    printf("  UDP Request/Reply succeeded: '%s'\n", reply);

    /* 2. Pulse transmission */
    rc = srr_udp_pulse(cli, SVC_UDP_ECHO, 5, "UDP_PULSE_DATA", 14);
    assert(rc == 0);
    printf("  UDP Pulse transmitted successfully.\n");

    /* 3. Timeout test */
    rc = srr_udp_request(cli, 999, 999, msg, strlen(msg), reply, sizeof(reply), &reply_len, 100);
    assert(rc == -1); /* Timeout */
    printf("  UDP Request timeout (100ms) verified.\n");

    /* Teardown */
    pthread_mutex_lock(&srv_ctx.lock);
    srv_ctx.running = false;
    pthread_mutex_unlock(&srv_ctx.lock);
    pthread_join(th, NULL);
    pthread_mutex_destroy(&srv_ctx.lock);
    srr_udp_close(cli);
    srr_udp_close(srv);
}

int main(void) {
    printf("=========================================\n");
    printf("SRR UDP Datagram Transport Tests\n");
    printf("=========================================\n");

    test_udp_roundtrip_and_pulse();

    printf("\nAll UDP transport tests PASSED!\n");
    return 0;
}
