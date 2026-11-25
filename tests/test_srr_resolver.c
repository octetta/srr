#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "srr/srr.h"
#include "srr_resolver.h"

/*
 * ============================================================================
 * Test 1: In-Memory Resolver Core (Register, Lookup, Unregister, Expiry)
 * ============================================================================
 */
static void test_resolver_in_memory(void) {
    printf("[TEST] In-memory registry operations...\n");
    srr_resolver_t *res = srr_resolver_create();
    assert(res != NULL);

    /* 1. Register static service */
    srr_endpoint_entry_t entry1 = {
        .service_name = "synth",
        .transport = SRR_TRANSPORT_IPC,
        .endpoint = "/tmp/srr_synth.sock",
        .baud_rate = 0,
        .service_id = 0,
        .ttl_seconds = 0
    };
    assert(srr_resolver_register(res, &entry1) == 0);

    /* 2. Register on-behalf-of serial device */
    srr_endpoint_entry_t entry2 = {
        .service_name = "pico-sensor",
        .transport = SRR_TRANSPORT_SERIAL,
        .endpoint = "/dev/ttyACM0",
        .baud_rate = 115200,
        .service_id = 0x1001,
        .ttl_seconds = 0
    };
    assert(srr_resolver_register(res, &entry2) == 0);

    /* 3. Register expiring UDP node (TTL 1 second) */
    srr_endpoint_entry_t entry3 = {
        .service_name = "esp32-node",
        .transport = SRR_TRANSPORT_UDP,
        .endpoint = "192.168.1.120:8888",
        .baud_rate = 0,
        .service_id = 0x2001,
        .ttl_seconds = 1
    };
    assert(srr_resolver_register(res, &entry3) == 0);

    /* 4. Lookup tests */
    srr_endpoint_entry_t out;
    assert(srr_resolver_lookup(res, "synth", &out) == 0);
    assert(out.transport == SRR_TRANSPORT_IPC);
    assert(strcmp(out.endpoint, "/tmp/srr_synth.sock") == 0);

    assert(srr_resolver_lookup(res, "pico-sensor", &out) == 0);
    assert(out.transport == SRR_TRANSPORT_SERIAL);
    assert(out.baud_rate == 115200);
    assert(out.service_id == 0x1001);

    assert(srr_resolver_lookup(res, "esp32-node", &out) == 0);
    assert(out.transport == SRR_TRANSPORT_UDP);

    assert(srr_resolver_lookup(res, "non-existent", &out) != 0);

    /* 5. Heartbeat & Expiration */
    usleep(1100000); /* 1.1s */
    /* esp32-node should be expired */
    assert(srr_resolver_lookup(res, "esp32-node", &out) != 0);

    /* static services must remain alive */
    assert(srr_resolver_lookup(res, "synth", &out) == 0);
    assert(srr_resolver_lookup(res, "pico-sensor", &out) == 0);

    /* 6. Unregister */
    assert(srr_resolver_unregister(res, "synth") == 0);
    assert(srr_resolver_lookup(res, "synth", &out) != 0);

    srr_resolver_destroy(res);
    printf("  -> In-memory registry operations PASSED.\n");
}

/*
 * ============================================================================
 * Test 2: Text Protocol Request/Reply Handler
 * ============================================================================
 */
static void test_resolver_text_protocol(void) {
    printf("[TEST] Resolver text protocol handler...\n");
    srr_resolver_t *res = srr_resolver_create();
    assert(res != NULL);

    char rep[1024];

    /* 1. Register serial proxy */
    assert(srr_resolver_handle_request(res, "REGISTER adc SERIAL /dev/ttyUSB0 115200 4097 0", rep, sizeof(rep)) == 0);
    assert(strstr(rep, "OK registered") != NULL);

    /* 2. Resolve */
    assert(srr_resolver_handle_request(res, "RESOLVE adc", rep, sizeof(rep)) == 0);
    assert(strstr(rep, "FOUND adc SERIAL /dev/ttyUSB0 115200 4097 0") != NULL);

    /* 3. List */
    assert(srr_resolver_handle_request(res, "LIST", rep, sizeof(rep)) == 0);
    assert(strstr(rep, "COUNT 1") != NULL);
    assert(strstr(rep, "ENTRY adc SERIAL") != NULL);

    /* 4. Unregister */
    assert(srr_resolver_handle_request(res, "UNREGISTER adc", rep, sizeof(rep)) == 0);
    assert(strstr(rep, "OK unregistered") != NULL);

    assert(srr_resolver_handle_request(res, "RESOLVE adc", rep, sizeof(rep)) == 0);
    assert(strstr(rep, "NOT_FOUND adc") != NULL);

    srr_resolver_destroy(res);
    printf("  -> Resolver text protocol handler PASSED.\n");
}

/*
 * ============================================================================
 * Test 3: End-to-End Client/Server IPC Resolution
 * ============================================================================
 */
typedef struct {
    srr_channel_t  *chan;
    srr_resolver_t *res;
    pthread_mutex_t lock;
    bool            running;
} server_thread_ctx_t;

static void* resolver_server_thread(void *arg) {
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    while (1) {
        pthread_mutex_lock(&ctx->lock);
        bool run = ctx->running;
        pthread_mutex_unlock(&ctx->lock);
        if (!run) break;

        srr_request_t *req = NULL;
        srr_status_t st = srr_receive(ctx->chan, &req);
        if (st == SRR_OK && req) {
            const char *data = (const char *)srr_request_data(req);
            char reply[1024] = {0};
            srr_resolver_handle_request(ctx->res, data, reply, sizeof(reply));
            srr_reply(req, reply, strlen(reply) + 1);
        }
    }
    return NULL;
}

static void test_resolver_ipc_roundtrip(void) {
    printf("[TEST] End-to-end client/server IPC resolution...\n");
    const char *svc_name = "test_resolver_svc";

    srr_resolver_t *res = srr_resolver_create();
    srr_channel_t *chan = NULL;
    srr_status_t st = srr_channel_create_service(svc_name, NULL, &chan);
    assert(st == SRR_OK);

    server_thread_ctx_t ctx = {
        .chan = chan,
        .res = res,
        .running = true
    };
    pthread_mutex_init(&ctx.lock, NULL);

    pthread_t th;
    pthread_create(&th, NULL, resolver_server_thread, &ctx);
    usleep(20000); /* 20ms */

    /* 1. Client registers service */
    srr_endpoint_entry_t reg_entry = {
        .service_name = "audio-synth",
        .transport = SRR_TRANSPORT_IPC,
        .endpoint = "/tmp/srr_synth.sock",
        .baud_rate = 0,
        .service_id = 0,
        .ttl_seconds = 0
    };
    assert(srr_register_service(svc_name, &reg_entry, 1000) == SRR_OK);

    /* 2. Client registers serial device on behalf of MCU */
    srr_endpoint_entry_t mcu_entry = {
        .service_name = "mcu-temp",
        .transport = SRR_TRANSPORT_SERIAL,
        .endpoint = "/dev/ttyACM0",
        .baud_rate = 115200,
        .service_id = 0x1002,
        .ttl_seconds = 0
    };
    assert(srr_register_service(svc_name, &mcu_entry, 1000) == SRR_OK);

    /* 3. Client resolves audio-synth */
    srr_endpoint_entry_t out;
    assert(srr_resolve(svc_name, "audio-synth", &out, 1000) == SRR_OK);
    assert(strcmp(out.service_name, "audio-synth") == 0);
    assert(out.transport == SRR_TRANSPORT_IPC);
    assert(strcmp(out.endpoint, "/tmp/srr_synth.sock") == 0);

    /* 4. Client resolves mcu-temp */
    assert(srr_resolve(svc_name, "mcu-temp", &out, 1000) == SRR_OK);
    assert(strcmp(out.service_name, "mcu-temp") == 0);
    assert(out.transport == SRR_TRANSPORT_SERIAL);
    assert(out.baud_rate == 115200);
    assert(out.service_id == 0x1002);

    /* 5. Client unregisters audio-synth */
    assert(srr_unregister_service(svc_name, "audio-synth", 1000) == SRR_OK);
    assert(srr_resolve(svc_name, "audio-synth", &out, 1000) != SRR_OK);

    /* Shutdown server */
    pthread_mutex_lock(&ctx.lock);
    ctx.running = false;
    pthread_mutex_unlock(&ctx.lock);
    srr_channel_close(chan);
    pthread_join(th, NULL);
    pthread_mutex_destroy(&ctx.lock);
    srr_channel_destroy(chan);
    srr_resolver_destroy(res);

    printf("  -> End-to-end client/server IPC resolution PASSED.\n");
}

int main(void) {
    printf("=========================================\n");
    printf("SRR Service & Device Resolver Tests\n");
    printf("=========================================\n");

    test_resolver_in_memory();
    test_resolver_text_protocol();
    test_resolver_ipc_roundtrip();

    printf("\nAll Resolver tests PASSED!\n");
    return 0;
}
