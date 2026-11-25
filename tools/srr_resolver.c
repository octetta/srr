#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include "srr/srr.h"
#include "srr_resolver.h"
#include "srr_udp.h"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

typedef struct {
    srr_resolver_t     *resolver;
    uint16_t           udp_port;
    bool               udp_enabled;
} udp_worker_args_t;

static void* udp_listener_thread(void *arg) {
    udp_worker_args_t *args = (udp_worker_args_t *)arg;
    srr_udp_config_t cfg;
    srr_udp_default_config(&cfg);
    cfg.enable_broadcast = true;
    cfg.default_timeout_ms = 500;

    srr_udp_endpoint_t *udp_server = srr_udp_server_create(args->udp_port, &cfg);
    if (!udp_server) {
        fprintf(stderr, "[RESOLVER] Warning: Could not bind UDP port %u\n", args->udp_port);
        return NULL;
    }

    printf("[RESOLVER] Listening for UDP discovery on port %u\n", args->udp_port);

    while (g_running) {
        srr_wire_header_t hdr;
        uint8_t payload[512] = {0};
        size_t actual_len = 0;
        srr_udp_peer_t *peer = NULL;

        int rc = srr_udp_server_receive(udp_server, &hdr, payload, sizeof(payload) - 1,
                                        &actual_len, &peer, 200);
        if (rc == 0 && peer) {
            payload[actual_len] = '\0';
            char reply[1024] = {0};
            srr_resolver_handle_request(args->resolver, (const char *)payload, reply, sizeof(reply));

            srr_udp_server_reply(udp_server, peer, hdr.trans_id,
                                 hdr.service_id, hdr.operation_id,
                                 reply, strlen(reply) + 1, false);
            srr_udp_peer_free(peer);
        }
    }

    srr_udp_close(udp_server);
    return NULL;
}

int main(int argc, char **argv) {
    const char *service_name = SRR_RESOLVER_DEFAULT_SERVICE;
    uint16_t udp_port = SRR_RESOLVER_DEFAULT_UDP_PORT;
    bool enable_udp = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--service") == 0 && i + 1 < argc) {
            service_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-udp") == 0) {
            enable_udp = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("SRR Service & Device Resolver Daemon\n\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --service <name>   IPC service name (default: %s)\n", SRR_RESOLVER_DEFAULT_SERVICE);
            printf("  --port <port>      UDP broadcast discovery port (default: %u)\n", SRR_RESOLVER_DEFAULT_UDP_PORT);
            printf("  --no-udp           Disable UDP network discovery listener\n");
            printf("  --help             Show this help reference\n");
            return 0;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    srr_resolver_t *res = srr_resolver_create();
    if (!res) {
        fprintf(stderr, "Failed to initialize resolver state\n");
        return 1;
    }

    srr_channel_t *chan = NULL;
    srr_status_t st = srr_channel_create_service(service_name, NULL, &chan);
    if (st != SRR_OK) {
        fprintf(stderr, "Failed to bind IPC service '%s' (status: %d)\n", service_name, st);
        srr_resolver_destroy(res);
        return 1;
    }

    printf("=========================================================\n");
    printf(" SRR Service & Device Resolver Daemon\n");
    printf(" IPC Service: '%s'\n", service_name);
    printf(" UDP Port:    %u (%s)\n", udp_port, enable_udp ? "ENABLED" : "DISABLED");
    printf("=========================================================\n");

    pthread_t udp_thread;
    udp_worker_args_t udp_args = { .resolver = res, .udp_port = udp_port, .udp_enabled = enable_udp };
    if (enable_udp) {
        pthread_create(&udp_thread, NULL, udp_listener_thread, &udp_args);
    }

    uint64_t last_prune_ms = current_time_ms();

    while (g_running) {
        srr_request_t *req = NULL;
        /* Timeout receive after 250ms to allow periodic housekeeping */
        srr_channel_options_t opts = { .queue_capacity = 64 };
        (void)opts;
        st = srr_receive(chan, &req);
        if (st == SRR_OK && req) {
            if (srr_request_is_pulse(req)) {
                /* Pulse can be heartbeat */
                srr_request_dispose(req);
                continue;
            }

            const char *data = (const char *)srr_request_data(req);
            char reply[2048] = {0};
            srr_resolver_handle_request(res, data, reply, sizeof(reply));

            srr_reply(req, reply, strlen(reply) + 1);
        }

        uint64_t now = current_time_ms();
        if (now - last_prune_ms > 1000) {
            srr_resolver_prune_expired(res, now);
            last_prune_ms = now;
        }
    }

    printf("\n[RESOLVER] Shutting down...\n");
    srr_channel_close(chan);
    srr_channel_destroy(chan);

    if (enable_udp) {
        pthread_join(udp_thread, NULL);
    }

    srr_resolver_destroy(res);
    printf("[RESOLVER] Clean exit.\n");
    return 0;
}
