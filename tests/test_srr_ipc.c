#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include "srr/srr.h"

#define SERVICE_NAME "test_srr_ipc_svc"

static void *ipc_server_worker(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            assert(srr_pulse_code(req) == 99);
            assert(srr_pulse_value(req) == 8888);
            srr_request_dispose(req);
            break; /* done */
        }

        const char *data = (const char *)srr_request_data(req);
        size_t size = srr_request_size(req);
        assert(size > 0);

        if (strcmp(data, "err_req") == 0) {
            srr_status_t rc = srr_reply_error(req, SRR_INVALID);
            assert(rc == SRR_OK);
        } else {
            char reply[128];
            snprintf(reply, sizeof(reply), "IPC_ECHO:%s", data);
            srr_status_t rc = srr_reply(req, reply, strlen(reply) + 1);
            assert(rc == SRR_OK);
        }
    }
    return NULL;
}

void test_ipc_roundtrip(void) {
    printf("[TEST] Process IPC (Unix Domain Socket) round-trip... ");

    srr_channel_options_t opts = {
        .queue_capacity = 32,
        .max_request_size = 4096,
        .max_reply_size = 4096,
        .service_name = SERVICE_NAME
    };

    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    pthread_t srv_th;
    pthread_create(&srv_th, NULL, ipc_server_worker, chan);

    /* Allow listener thread to bind */
    struct timespec ts = {0, 20 * 1000000L};
    nanosleep(&ts, NULL);

    /* Client connects by service name */
    srr_connection_t *conn = NULL;
    assert(srr_connect(SERVICE_NAME, NULL, &conn) == SRR_OK);

    /* 1. Send normal request */
    const char *msg = "hello from ipc client";
    char reply[128] = {0};
    size_t reply_sz = 0;
    srr_status_t st = srr_send(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz);
    assert(st == SRR_OK);
    assert(strcmp(reply, "IPC_ECHO:hello from ipc client") == 0);
    assert(reply_sz == strlen(reply) + 1);

    /* 2. Error reply */
    st = srr_send(conn, "err_req", 8, reply, sizeof(reply), &reply_sz);
    assert(st == SRR_INVALID);

    /* 3. Pulse */
    assert(srr_pulse(conn, 99, 8888) == SRR_OK);

    pthread_join(srv_th, NULL);
    srr_disconnect(conn);
    srr_channel_destroy(chan);
    printf("PASS\n");
}

void test_forked_process_ipc(void) {
    printf("[TEST] Cross-process IPC via fork()... ");
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
    printf("[SKIP: fork() without exec() after threads is not supported by ThreadSanitizer runtime]\n");
    return;
#endif
    const char *svc_name = "test_srr_fork_svc";

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child Process: connects with retry to wait for parent listener to bind */
        srr_connection_t *conn = NULL;
        srr_status_t cst = SRR_DISCONNECTED;
        for (int retry = 0; retry < 50; retry++) {
            struct timespec ts = {0, 10 * 1000000L}; /* 10ms */
            nanosleep(&ts, NULL);
            cst = srr_connect(svc_name, NULL, &conn);
            if (cst == SRR_OK) break;
        }
        assert(cst == SRR_OK);

        const char *msg = "ping from child process";
        char reply[64] = {0};
        size_t reply_sz = 0;
        srr_status_t st = srr_send(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz);
        assert(st == SRR_OK);
        assert(strcmp(reply, "PONG:ping from child process") == 0);

        srr_disconnect(conn);
        exit(0);
    } else {
        /* Parent Process: creates server channel, receives and replies */
        srr_channel_options_t opts = {
            .queue_capacity = 16,
            .service_name = svc_name
        };
        srr_channel_t *chan = NULL;
        assert(srr_channel_create(&opts, &chan) == SRR_OK);

        srr_request_t *req = NULL;
        srr_status_t st = srr_receive(chan, &req);
        assert(st == SRR_OK);

        const char *data = (const char *)srr_request_data(req);
        char reply[64];
        snprintf(reply, sizeof(reply), "PONG:%s", data);
        srr_status_t rc = srr_reply(req, reply, strlen(reply) + 1);
        assert(rc == SRR_OK);

        int status = 0;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        srr_channel_destroy(chan);
        printf("PASS\n");
    }
}

int main(void) {
    printf("=== SRR Process IPC (Unix Domain Socket) Tests ===\n");
    test_ipc_roundtrip();
    test_forked_process_ipc();
    printf("All IPC tests passed!\n");
    return 0;
}
