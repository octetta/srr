#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <windows.h>
#include "srr/srr.h"

#define PIPE_SVC_NAME "win32_srr_pipe_test"

static DWORD WINAPI win32_pipe_server(LPVOID arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            assert(srr_pulse_code(req) == 77);
            assert(srr_pulse_value(req) == 9999);
            srr_request_dispose(req);
            break;
        }

        const char *data = (const char *)srr_request_data(req);
        char reply[128];
        snprintf(reply, sizeof(reply), "WIN_ECHO:%s", data);
        srr_reply(req, reply, strlen(reply) + 1);
    }
    return 0;
}

int main(void) {
    printf("=== SRR Windows Named Pipe Tests ===\n");
    srr_channel_options_t opts = {
        .queue_capacity = 16,
        .service_name = PIPE_SVC_NAME
    };

    srr_channel_t *chan = NULL;
    assert(srr_channel_create(&opts, &chan) == SRR_OK);

    HANDLE hTh = CreateThread(NULL, 0, win32_pipe_server, chan, 0, NULL);
    assert(hTh != NULL);

    Sleep(50); /* Allow server pipe to create */

    srr_connection_t *conn = NULL;
    assert(srr_connect(PIPE_SVC_NAME, NULL, &conn) == SRR_OK);

    /* 1. Send/Reply over Named Pipe */
    const char *msg = "hello windows named pipe";
    char reply[64] = {0};
    size_t reply_sz = 0;
    srr_status_t st = srr_send(conn, msg, strlen(msg) + 1, reply, sizeof(reply), &reply_sz);
    assert(st == SRR_OK);
    assert(strcmp(reply, "WIN_ECHO:hello windows named pipe") == 0);
    printf("[TEST] Windows Named Pipe Send/Reply... PASS\n");

    /* 2. Pulse over Named Pipe */
    assert(srr_pulse(conn, 77, 9999) == SRR_OK);
    printf("[TEST] Windows Named Pipe Pulse... PASS\n");

    WaitForSingleObject(hTh, INFINITE);
    CloseHandle(hTh);
    srr_disconnect(conn);
    srr_channel_destroy(chan);

    printf("All Windows Named Pipe tests passed!\n");
    return 0;
}
#else
int main(void) { return 0; }
#endif
