#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "srr/srr.h"

#define BENCH_WARMUP_ITERS 1000
#define BENCH_ITERS 50000

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_uint64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

typedef struct {
    uint64_t min_ns;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
    double   ops_per_sec;
    double   total_sec;
} bench_stats_t;

static void compute_stats(uint64_t *latencies, size_t count, uint64_t total_ns, bench_stats_t *out) {
    qsort(latencies, count, sizeof(uint64_t), cmp_uint64);
    out->min_ns = latencies[0];
    out->p50_ns = latencies[count * 50 / 100];
    out->p95_ns = latencies[count * 95 / 100];
    out->p99_ns = latencies[count * 99 / 100];
    out->max_ns = latencies[count - 1];
    out->total_sec = (double)total_ns / 1e9;
    out->ops_per_sec = (double)count / out->total_sec;
}

static void print_stats(const char *name, const bench_stats_t *s, size_t count) {
    printf("======================================================================\n");
    printf(" Benchmark: %s\n", name);
    printf(" Iterations: %zu | Total Time: %.3f s | Throughput: %.0f ops/sec\n",
           count, s->total_sec, s->ops_per_sec);
    printf(" Latency (Round-Trip):\n");
    printf("   Min: %.2f µs | p50: %.2f µs | p95: %.2f µs | p99: %.2f µs | Max: %.2f µs\n",
           (double)s->min_ns / 1000.0,
           (double)s->p50_ns / 1000.0,
           (double)s->p95_ns / 1000.0,
           (double)s->p99_ns / 1000.0,
           (double)s->max_ns / 1000.0);
    printf("======================================================================\n\n");
}

/*
 * ----------------------------------------------------------------------------
 * 1. In-Process Thread Benchmark (1 Client, 1 Server)
 * ----------------------------------------------------------------------------
 */

static void *bench_echo_server(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            srr_request_dispose(req);
            break;
        }
        const void *data = srr_request_data(req);
        size_t sz = srr_request_size(req);
        srr_reply(req, data, sz);
    }
    return NULL;
}

static void run_bench_pthread_1to1(void) {
    srr_channel_options_t opts = {
        .queue_capacity = 256,
        .max_request_size = 1024,
        .max_reply_size = 1024
    };
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    pthread_t srv_th;
    pthread_create(&srv_th, NULL, bench_echo_server, chan);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    char payload[64] = "benchmark_small_payload_64_bytes_test_message_for_latency_test";
    char reply[64];
    size_t reply_sz = 0;

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        srr_send(conn, payload, sizeof(payload), reply, sizeof(reply), &reply_sz);
    }

    /* Timed Runs */
    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * BENCH_ITERS);
    assert(latencies != NULL);

    uint64_t t_start = get_time_ns();
    for (size_t i = 0; i < BENCH_ITERS; i++) {
        uint64_t req_start = get_time_ns();
        srr_send(conn, payload, sizeof(payload), reply, sizeof(reply), &reply_sz);
        uint64_t req_end = get_time_ns();
        latencies[i] = req_end - req_start;
    }
    uint64_t t_end = get_time_ns();

    bench_stats_t stats;
    compute_stats(latencies, BENCH_ITERS, t_end - t_start, &stats);
    print_stats("Pthread In-Process (1 Client -> 1 Server, 64-byte payload)", &stats, BENCH_ITERS);

    /* Clean exit */
    srr_pulse(conn, 1, 0);
    pthread_join(srv_th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    free(latencies);
}

/*
 * ----------------------------------------------------------------------------
 * 2. In-Process Thread Benchmark (4 Clients, 4 Servers)
 * ----------------------------------------------------------------------------
 */

#define M2M_CLIENTS 4
#define M2M_SERVERS 4
#define M2M_ITERS_PER_CLIENT 15000

typedef struct {
    srr_channel_t *chan;
    uint64_t      *latencies;
    size_t         count;
} client_bench_arg_t;

static void *bench_client_worker(void *arg) {
    client_bench_arg_t *carg = (client_bench_arg_t *)arg;
    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(carg->chan, NULL, &conn) == SRR_OK);

    char payload[64] = "m2m_payload_64_bytes_test_message_for_contention_benchmark_0001";
    char reply[64];
    size_t reply_sz = 0;

    for (size_t i = 0; i < carg->count; i++) {
        uint64_t req_start = get_time_ns();
        srr_send(conn, payload, sizeof(payload), reply, sizeof(reply), &reply_sz);
        uint64_t req_end = get_time_ns();
        carg->latencies[i] = req_end - req_start;
    }

    srr_disconnect(conn);
    return NULL;
}

static void run_bench_pthread_m2m(void) {
    srr_channel_options_t opts = {
        .queue_capacity = 512,
        .max_request_size = 1024,
        .max_reply_size = 1024
    };
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    pthread_t servers[M2M_SERVERS];
    for (int i = 0; i < M2M_SERVERS; i++) {
        pthread_create(&servers[i], NULL, bench_echo_server, chan);
    }

    pthread_t clients[M2M_CLIENTS];
    client_bench_arg_t cargs[M2M_CLIENTS];
    for (int i = 0; i < M2M_CLIENTS; i++) {
        cargs[i].chan = chan;
        cargs[i].count = M2M_ITERS_PER_CLIENT;
        cargs[i].latencies = (uint64_t *)malloc(sizeof(uint64_t) * M2M_ITERS_PER_CLIENT);
    }

    uint64_t t_start = get_time_ns();
    for (int i = 0; i < M2M_CLIENTS; i++) {
        pthread_create(&clients[i], NULL, bench_client_worker, &cargs[i]);
    }

    for (int i = 0; i < M2M_CLIENTS; i++) {
        pthread_join(clients[i], NULL);
    }
    uint64_t t_end = get_time_ns();

    size_t total_count = M2M_CLIENTS * M2M_ITERS_PER_CLIENT;
    uint64_t *all_lats = (uint64_t *)malloc(sizeof(uint64_t) * total_count);
    size_t offset = 0;
    for (int i = 0; i < M2M_CLIENTS; i++) {
        memcpy(all_lats + offset, cargs[i].latencies, sizeof(uint64_t) * M2M_ITERS_PER_CLIENT);
        offset += M2M_ITERS_PER_CLIENT;
        free(cargs[i].latencies);
    }

    bench_stats_t stats;
    compute_stats(all_lats, total_count, t_end - t_start, &stats);
    print_stats("Pthread In-Process Multi-Threaded (4 Clients -> 4 Servers, 64-byte payload)", &stats, total_count);

    /* Wake servers and close */
    assert(srr_channel_close(chan) == SRR_OK);
    for (int i = 0; i < M2M_SERVERS; i++) {
        pthread_join(servers[i], NULL);
    }
    srr_channel_destroy(chan);
    free(all_lats);
}

/*
 * ----------------------------------------------------------------------------
 * 3. Process IPC Benchmark over Unix Domain Sockets (1 Client -> 1 Server)
 * ----------------------------------------------------------------------------
 */

#define IPC_BENCH_ITERS 30000

static void run_bench_ipc_1to1(void) {
    const char *svc = "srr_bench_uds_service";
    srr_channel_options_t opts = {
        .queue_capacity = 256,
        .max_request_size = 1024,
        .max_reply_size = 1024,
        .service_name = svc
    };
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    pthread_t srv_th;
    pthread_create(&srv_th, NULL, bench_echo_server, chan);

    struct timespec ts = {0, 30 * 1000000L};
    nanosleep(&ts, NULL);

    srr_connection_t *conn = NULL;
    assert(srr_connect(svc, NULL, &conn) == SRR_OK);

    char payload[64] = "ipc_benchmark_small_payload_64_bytes_test_message_for_socket_t";
    char reply[64];
    size_t reply_sz = 0;

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        srr_send(conn, payload, sizeof(payload), reply, sizeof(reply), &reply_sz);
    }

    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * IPC_BENCH_ITERS);
    assert(latencies != NULL);

    uint64_t t_start = get_time_ns();
    for (size_t i = 0; i < IPC_BENCH_ITERS; i++) {
        uint64_t req_start = get_time_ns();
        srr_send(conn, payload, sizeof(payload), reply, sizeof(reply), &reply_sz);
        uint64_t req_end = get_time_ns();
        latencies[i] = req_end - req_start;
    }
    uint64_t t_end = get_time_ns();

    bench_stats_t stats;
    compute_stats(latencies, IPC_BENCH_ITERS, t_end - t_start, &stats);
    print_stats("Local Process IPC / Unix Domain Sockets (1 Client -> 1 Server, 64-byte payload)", &stats, IPC_BENCH_ITERS);

    srr_pulse(conn, 1, 0);
    pthread_join(srv_th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    free(latencies);
}

int main(void) {
    printf("\n######################################################################\n");
    printf("                  SRR PERFORMANCE BENCHMARK SUITE                     \n");
    printf("######################################################################\n\n");

    run_bench_pthread_1to1();
    run_bench_pthread_m2m();
    run_bench_ipc_1to1();

    printf("Benchmark suite completed successfully.\n");
    return 0;
}
