#define _POSIX_C_SOURCE 200809L
#include "srr_serial.h"
#include "srr_embedded.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <assert.h>
#include <signal.h>

#define SVC_ECHO  1
#define OP_ECHO   10
#define SVC_DELAY 2
#define OP_SLEEP  20

static int server_fd_write(const uint8_t *data, size_t len, void *user_data) {
    int fd = *(int *)user_data;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static int echo_service_handler(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *req_payload,
    size_t req_len,
    uint8_t *reply_buf,
    size_t reply_cap,
    size_t *out_reply_len,
    void *user_data)
{
    (void)service_id;
    (void)user_data;
    if (operation_id == OP_ECHO) {
        if (reply_cap < req_len) return -1;
        memcpy(reply_buf, req_payload, req_len);
        *out_reply_len = req_len;
        return 0;
    }
    return -1;
}

typedef struct {
    int server_fd;
    pthread_mutex_t lock;
    bool running;
    srr_endpoint_t ep;
    uint8_t rx_buf[1024];
    uint8_t dec_buf[1024];
    uint8_t tx_buf[1024];
    uint8_t rep_buf[1024];
} server_thread_ctx_t;

static void* server_thread_fn(void *arg) {
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    uint8_t ch;
    while (1) {
        pthread_mutex_lock(&ctx->lock);
        bool run = ctx->running;
        pthread_mutex_unlock(&ctx->lock);
        if (!run) break;

        ssize_t n = read(ctx->server_fd, &ch, 1);
        if (n <= 0) break; /* Closed or error */
        srr_endpoint_feed_byte(&ctx->ep, ch);
    }
    return NULL;
}

static void test_serial_roundtrip_and_pulse(void) {
    printf("Testing serial transport request/reply and pulses...\n");

    int fds[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);

    /* Setup embedded endpoint on server_fd (fds[1]) */
    server_thread_ctx_t srv_ctx;
    memset(&srv_ctx, 0, sizeof(srv_ctx));
    srv_ctx.server_fd = fds[1];
    pthread_mutex_init(&srv_ctx.lock, NULL);
    srv_ctx.running = true;

    rc = srr_endpoint_init(
        &srv_ctx.ep, 0x1001, server_fd_write, &srv_ctx.server_fd,
        srv_ctx.rx_buf, sizeof(srv_ctx.rx_buf),
        srv_ctx.dec_buf, sizeof(srv_ctx.dec_buf),
        srv_ctx.tx_buf, sizeof(srv_ctx.tx_buf),
        srv_ctx.rep_buf, sizeof(srv_ctx.rep_buf)
    );
    assert(rc == 0);
    srr_endpoint_register_service(&srv_ctx.ep, SVC_ECHO, echo_service_handler, NULL);

    pthread_t th;
    pthread_create(&th, NULL, server_thread_fn, &srv_ctx);

    /* Setup client serial port on fds[0] */
    srr_serial_port_t *client_port = srr_serial_from_fd(fds[0], true, NULL);
    assert(client_port != NULL);

    /* Test 1: Synchronous Request/Reply */
    const char *msg = "Hello serial wire!";
    char reply[128] = {0};
    size_t actual_reply_len = 0;

    rc = srr_serial_request(
        client_port, SVC_ECHO, OP_ECHO,
        msg, strlen(msg),
        reply, sizeof(reply) - 1,
        &actual_reply_len, 2000
    );
    assert(rc == 0);
    assert(actual_reply_len == strlen(msg));
    assert(strcmp(reply, msg) == 0);
    printf("  Request/reply roundtrip verified: '%s'\n", reply);

    /* Test 2: Unknown service returns error */
    rc = srr_serial_request(
        client_port, 999, 999,
        msg, strlen(msg),
        reply, sizeof(reply) - 1,
        &actual_reply_len, 2000
    );
    assert(rc == -3); /* Returned ERROR frame */
    printf("  Unknown service error response verified.\n");

    /* Test 3: One-way pulse */
    rc = srr_serial_pulse(client_port, SVC_ECHO, 5, "PULSE_DATA", 10);
    assert(rc == 0);
    printf("  One-way pulse sent successfully.\n");

    /* Teardown */
    pthread_mutex_lock(&srv_ctx.lock);
    srv_ctx.running = false;
    pthread_mutex_unlock(&srv_ctx.lock);
    shutdown(fds[1], SHUT_RDWR);
    pthread_join(th, NULL);
    pthread_mutex_destroy(&srv_ctx.lock);
    close(fds[1]);
    srr_serial_close(client_port);
}

static void test_serial_timeout_and_disconnect(void) {
    printf("Testing serial timeout and link loss...\n");

    int fds[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);

    srr_serial_port_t *client_port = srr_serial_from_fd(fds[0], true, NULL);
    assert(client_port != NULL);

    /* 1. Timeout test: Server doesn't respond */
    char reply[64];
    size_t reply_len = 0;
    rc = srr_serial_request(
        client_port, SVC_DELAY, OP_SLEEP,
        "PING", 4,
        reply, sizeof(reply),
        &reply_len, 100 /* 100ms timeout */
    );
    assert(rc == -1); /* Timeout */
    printf("  Serial request timeout (100ms) verified.\n");

    /* 2. Link disconnect test: Close remote end */
    close(fds[1]);
    rc = srr_serial_request(
        client_port, SVC_DELAY, OP_SLEEP,
        "PING", 4,
        reply, sizeof(reply),
        &reply_len, 500
    );
    assert(rc == -2); /* Link disconnected */
    printf("  Serial link disconnect detected correctly.\n");

    srr_serial_close(client_port);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("=========================================\n");
    printf("SRR Serial Transport Unit Test Suite\n");
    printf("=========================================\n");

    test_serial_roundtrip_and_pulse();
    test_serial_timeout_and_disconnect();

    printf("\nAll serial transport tests PASSED!\n");
    return 0;
}
