#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "srr/srr.h"

/*
 * Example: Zero-Copy Scatter-Gather I/O (srr_sendv & srr_replyv)
 *
 * Demonstrates sending structured command headers and separate bulk payloads
 * across process/thread boundaries without allocating intermediate staging
 * buffers or paying for redundant memcpy operations.
 */

/* 1. Protocol Data Structures */
typedef struct {
    uint32_t opcode;       /* 0x01 = SET_CALIBRATION */
    uint32_t sensor_id;
    uint64_t timestamp_ns;
} cmd_header_t;

typedef struct {
    float calibration_matrix[4][4];
} calib_payload_t;

typedef struct {
    int32_t  status_code;  /* 0 = OK */
    uint32_t ack_seq;
} reply_status_t;

typedef struct {
    float current_temperature_c;
    float current_voltage;
} reply_telemetry_t;

/* 2. Server Worker Loop */
static void *server_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;
    srr_request_t *req = NULL;

    printf("[Server] Ready and waiting for scatter-gather requests...\n");

    while (srr_receive(chan, &req) == SRR_OK) {
        if (srr_request_is_pulse(req)) {
            srr_request_dispose(req);
            break;
        }

        /* Server receives the combined payload as a single contiguous slice */
        size_t total_size = srr_request_size(req);
        const uint8_t *raw = (const uint8_t *)srr_request_data(req);

        if (total_size >= sizeof(cmd_header_t) + sizeof(calib_payload_t)) {
            const cmd_header_t *hdr = (const cmd_header_t *)raw;
            const calib_payload_t *calib = (const calib_payload_t *)(raw + sizeof(cmd_header_t));

            printf("[Server] Received Cmd Opcode: 0x%02X, Sensor: %u, Matrix[0][0]: %.2f\n",
                   hdr->opcode, hdr->sensor_id, calib->calibration_matrix[0][0]);

            /* Server replies using 2 non-contiguous vectors: status + telemetry */
            reply_status_t status = { .status_code = 0, .ack_seq = 1001 };
            reply_telemetry_t telem = { .current_temperature_c = 24.5f, .current_voltage = 3.31f };

            srr_iovec_t rep_iov[2] = {
                { .iov_base = &status, .iov_len = sizeof(status) },
                { .iov_base = &telem,  .iov_len = sizeof(telem)  }
            };

            /* Single atomic reply call across disjoint memory regions */
            srr_replyv(req, rep_iov, 2);
        } else {
            srr_reply_error(req, SRR_INVALID);
        }
    }
    return NULL;
}

int main(void) {
    printf("=== SRR Zero-Copy Scatter-Gather I/O Example ===\n\n");

    const char *service_name = "example_scatter_service";

    /* Create IPC service channel */
    srr_channel_options_t opts = {
        .queue_capacity = 64,
        .max_request_size = 4096,
        .max_reply_size = 4096,
        .service_name = service_name
    };

    srr_channel_t *chan = NULL;
    srr_channel_create_service(service_name, &opts, &chan);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, chan);

    /* Allow server socket to bind */
    struct timespec ts = {0, 20000000L};
    nanosleep(&ts, NULL);

    /* Client connects to service */
    srr_connection_t *conn = NULL;
    srr_connect(service_name, NULL, &conn);

    /* Client prepares separate header and data structs in disjoint stack memory */
    cmd_header_t cmd = {
        .opcode = 0x01,
        .sensor_id = 42,
        .timestamp_ns = 1700000000ULL
    };

    calib_payload_t calib;
    memset(&calib, 0, sizeof(calib));
    calib.calibration_matrix[0][0] = 1.05f;

    /* Gather list for transmission */
    srr_iovec_t send_iov[2] = {
        { .iov_base = &cmd,   .iov_len = sizeof(cmd)   },
        { .iov_base = &calib, .iov_len = sizeof(calib) }
    };

    /* Scatter list for reception */
    reply_status_t rep_status = {0};
    reply_telemetry_t rep_telem = {0};

    srr_iovec_t reply_iov[2] = {
        { .iov_base = &rep_status, .iov_len = sizeof(rep_status) },
        { .iov_base = &rep_telem,  .iov_len = sizeof(rep_telem)  }
    };

    size_t out_reply_size = 0;
    printf("[Client] Sending 2-slice request via srr_sendv()...\n");
    srr_status_t rc = srr_sendv(conn, send_iov, 2, reply_iov, 2, &out_reply_size);

    if (rc == SRR_OK) {
        printf("[Client] Reply received! (%zu bytes total)\n", out_reply_size);
        printf("[Client] Status Code: %d, Ack Seq: %u\n", rep_status.status_code, rep_status.ack_seq);
        printf("[Client] Telemetry: Temp = %.1f C, Voltage = %.2f V\n",
               rep_telem.current_temperature_c, rep_telem.current_voltage);
    } else {
        printf("[Client] Transaction failed with status: %d\n", rc);
    }

    /* Shutdown */
    srr_disconnect(conn);
    srr_channel_close(chan);
    pthread_join(th, NULL);
    srr_channel_destroy(chan);

    printf("\nExample finished successfully.\n");
    return 0;
}
