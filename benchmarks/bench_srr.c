#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "srr/srr.h"
#include "srr_wire.h"
#include "srr_serial.h"
#include "srr_embedded.h"

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

/*
 * ----------------------------------------------------------------------------
 * 4. Queue-Full & Backpressure Benchmark (§15 requirement)
 * ----------------------------------------------------------------------------
 */

static void run_bench_queue_full(void) {
    srr_channel_options_t opts = {
        .queue_capacity = 32, /* Small queue to trigger full state */
        .max_request_size = 256,
        .max_reply_size = 256
    };
    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    srr_connection_t *conn = NULL;
    assert(srr_connect_channel(chan, NULL, &conn) == SRR_OK);

    /* Fill queue with pulses without reading */
    size_t filled = 0;
    while (srr_pulse(conn, 1, 42) == SRR_OK) {
        filled++;
    }
    assert(filled == 32);

    /* Queue is now full: measure rejection latency */
    size_t iters = 50000;
    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * iters);

    uint64_t t_start = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
        uint64_t s = get_time_ns();
        srr_status_t st = srr_pulse(conn, 1, 42);
        uint64_t e = get_time_ns();
        assert(st == SRR_QUEUE_FULL);
        latencies[i] = e - s;
    }
    uint64_t t_end = get_time_ns();

    bench_stats_t stats;
    compute_stats(latencies, iters, t_end - t_start, &stats);
    print_stats("Queue-Full / Backpressure Rejection (Capacity 32, SRR_QUEUE_FULL)", &stats, iters);

    /* Drain queue & destroy */
    srr_disconnect(conn);
    srr_channel_close(chan);
    srr_channel_destroy(chan);
    free(latencies);
}

/*
 * ----------------------------------------------------------------------------
 * 5. Wire Framing (COBS + CRC-16) Encoding & Decoding Benchmark
 * ----------------------------------------------------------------------------
 */

static void run_bench_wire_framing(void) {
    srr_wire_header_t hdr = {
        .magic = SRR_WIRE_MAGIC,
        .version = SRR_WIRE_VERSION,
        .msg_type = SRR_WIRE_REQUEST,
        .session_id = 0x1234,
        .trans_id = 999,
        .service_id = 1,
        .operation_id = 2,
        .payload_len = 64
    };

    uint8_t payload[64];
    memset(payload, 0x5A, sizeof(payload));

    uint8_t encoded[256];
    size_t iters = 100000;

    /* Benchmark full frame encoding */
    uint64_t *enc_lats = (uint64_t *)malloc(sizeof(uint64_t) * iters);
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
        uint64_t s = get_time_ns();
        size_t len = srr_wire_encode_frame(&hdr, payload, sizeof(payload), encoded, sizeof(encoded));
        uint64_t e = get_time_ns();
        (void)len;
        enc_lats[i] = e - s;
    }
    uint64_t t1 = get_time_ns();

    bench_stats_t enc_stats;
    compute_stats(enc_lats, iters, t1 - t0, &enc_stats);
    print_stats("Wire Framing: Encode Frame (32B Hdr + 64B Payload + CRC16 + COBS)", &enc_stats, iters);

    /* Benchmark streaming chunk parser decode */
    uint8_t rx_buf[256];
    uint8_t dec_buf[256];
    srr_wire_parser_t parser;
    srr_wire_parser_init(&parser, rx_buf, sizeof(rx_buf), dec_buf, sizeof(dec_buf));

    size_t frame_len = srr_wire_encode_frame(&hdr, payload, sizeof(payload), encoded, sizeof(encoded));
    uint64_t *dec_lats = (uint64_t *)malloc(sizeof(uint64_t) * iters);

    t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
        srr_wire_header_t out_hdr;
        const uint8_t *out_p = NULL;
        uint64_t s = get_time_ns();
        for (size_t b = 0; b < frame_len; b++) {
            srr_wire_parser_feed_byte(&parser, encoded[b], &out_hdr, &out_p);
        }
        uint64_t e = get_time_ns();
        dec_lats[i] = e - s;
    }
    t1 = get_time_ns();

    bench_stats_t dec_stats;
    compute_stats(dec_lats, iters, t1 - t0, &dec_stats);
    print_stats("Wire Framing: Decode 1-Byte Chunk Feed (64B Payload + Header + CRC)", &dec_stats, iters);

    free(enc_lats);
    free(dec_lats);
}

/*
 * ----------------------------------------------------------------------------
 * 6. Serial Transport Loopback Benchmark
 * ----------------------------------------------------------------------------
 */

static int serial_bench_tx(const uint8_t *data, size_t len, void *user_data) {
    int fd = *(int *)user_data;
    ssize_t n = write(fd, data, len);
    return n == (ssize_t)len ? 0 : -1;
}

static int serial_bench_handler(
    uint16_t svc, uint16_t op,
    const uint8_t *req, size_t req_len,
    uint8_t *reply, size_t cap, size_t *out_len,
    void *user_data)
{
    (void)svc; (void)op; (void)user_data;
    if (cap < req_len) return -1;
    memcpy(reply, req, req_len);
    *out_len = req_len;
    return 0;
}

typedef struct {
    int srv_fd;
    volatile bool running;
    srr_endpoint_t ep;
    uint8_t rx[512], dec[512], tx[512], rep[512];
} serial_bench_srv_t;

static void* serial_srv_worker(void *arg) {
    serial_bench_srv_t *ctx = (serial_bench_srv_t *)arg;
    uint8_t ch;
    while (ctx->running) {
        ssize_t n = read(ctx->srv_fd, &ch, 1);
        if (n <= 0) break;
        srr_endpoint_feed_byte(&ctx->ep, ch);
    }
    return NULL;
}

static void run_bench_serial_loopback(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    serial_bench_srv_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.srv_fd = fds[1];
    srv.running = true;

    assert(srr_endpoint_init(&srv.ep, 0x5555, serial_bench_tx, &srv.srv_fd,
                             srv.rx, sizeof(srv.rx), srv.dec, sizeof(srv.dec),
                             srv.tx, sizeof(srv.tx), srv.rep, sizeof(srv.rep)) == 0);
    srr_endpoint_register_service(&srv.ep, 1, serial_bench_handler, NULL);

    pthread_t th;
    pthread_create(&th, NULL, serial_srv_worker, &srv);

    srr_serial_port_t *client = srr_serial_from_fd(fds[0], true, NULL);
    assert(client != NULL);

    char payload[32] = "serial_bench_32_bytes_payload!!";
    char reply[64];
    size_t reply_len = 0;

    size_t iters = 5000;
    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * iters);

    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
        uint64_t s = get_time_ns();
        int rc = srr_serial_request(client, 1, 1, payload, sizeof(payload), reply, sizeof(reply), &reply_len, 1000);
        uint64_t e = get_time_ns();
        assert(rc == 0);
        latencies[i] = e - s;
    }
    uint64_t t1 = get_time_ns();

    bench_stats_t stats;
    compute_stats(latencies, iters, t1 - t0, &stats);
    print_stats("Serial Wire Full-Duplex Loopback (32-byte payload, End-to-End)", &stats, iters);

    srv.running = false;
    shutdown(fds[1], SHUT_RDWR);
    pthread_join(th, NULL);
    close(fds[1]);
    srr_serial_close(client);
    free(latencies);
}

int main(void) {
    printf("\n######################################################################\n");
    printf("                  SRR PERFORMANCE BENCHMARK SUITE                     \n");
    printf("######################################################################\n\n");

    run_bench_pthread_1to1();
    run_bench_pthread_m2m();
    run_bench_ipc_1to1();
    run_bench_queue_full();
    run_bench_wire_framing();
    run_bench_serial_loopback();

    printf("Benchmark suite completed successfully.\n");
    return 0;
}
