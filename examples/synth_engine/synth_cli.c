#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "srr/srr.h"

#define SYNTH_SERVICE_NAME "srr_synth"

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void send_cmd(srr_connection_t *conn, const char *cmd) {
    char reply[512] = {0};
    size_t reply_sz = 0;

    uint64_t t0 = get_time_ns();
    srr_status_t st = srr_send_timed(conn, cmd, strlen(cmd) + 1, reply, sizeof(reply), &reply_sz, 2000);
    uint64_t t1 = get_time_ns();

    if (st == SRR_OK) {
        printf("<- %s (latency: %.1f µs)\n", reply, (double)(t1 - t0) / 1000.0);
    } else {
        printf("<- ERROR [%s] (latency: %.1f µs)\n", srr_status_str(st), (double)(t1 - t0) / 1000.0);
    }
}

int main(int argc, char **argv) {
    const char *service_name = SYNTH_SERVICE_NAME;
    srr_connection_t *conn = NULL;

    srr_status_t st = srr_connect(service_name, NULL, &conn);
    if (st != SRR_OK) {
        fprintf(stderr, "Failed to connect to synth service '%s': %s\n", service_name, srr_status_str(st));
        return 1;
    }

    /* One-shot command line mode */
    if (argc > 1) {
        char cmd[512] = {0};
        if (strcmp(argv[1], "tick") == 0) {
            uint64_t count = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
            st = srr_pulse(conn, 1, count);
            printf("Sent MIDI clock pulse (code=1, val=%lu): %s\n", (unsigned long)count, srr_status_str(st));
            srr_disconnect(conn);
            return 0;
        }

        if (strcmp(argv[1], "status") == 0) {
            snprintf(cmd, sizeof(cmd), "STATUS");
        } else if (strcmp(argv[1], "set") == 0 && argc >= 4) {
            snprintf(cmd, sizeof(cmd), "SET %s %s", argv[2], argv[3]);
        } else if (strcmp(argv[1], "get") == 0 && argc >= 3) {
            snprintf(cmd, sizeof(cmd), "GET %s", argv[2]);
        } else if (strcmp(argv[1], "note-on") == 0 && argc >= 4) {
            snprintf(cmd, sizeof(cmd), "NOTE_ON %s %s", argv[2], argv[3]);
        } else if (strcmp(argv[1], "note-off") == 0 && argc >= 3) {
            snprintf(cmd, sizeof(cmd), "NOTE_OFF %s", argv[2]);
        } else if (strcmp(argv[1], "shutdown") == 0) {
            snprintf(cmd, sizeof(cmd), "SHUTDOWN");
        } else {
            /* Pass raw command string */
            for (int i = 1; i < argc; i++) {
                strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
                if (i < argc - 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            }
        }

        printf("-> %s\n", cmd);
        send_cmd(conn, cmd);
        srr_disconnect(conn);
        return 0;
    }

    /* Interactive REPL mode */
    printf("Connected to SRR Synthesizer Service '%s'. Type 'help' or 'quit'.\n", service_name);
    char line[256];

    while (1) {
        printf("srr_synth> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        if (strcmp(line, "help") == 0) {
            printf("Commands:\n");
            printf("  status\n");
            printf("  set <param> <value>   (e.g. set cutoff 1200)\n");
            printf("  get <param>           (e.g. get cutoff)\n");
            printf("  note-on <note> <vel>  (e.g. note-on 60 100)\n");
            printf("  note-off <note>       (e.g. note-off 60)\n");
            printf("  tick [n]              (sends asynchronous clock pulse)\n");
            printf("  shutdown\n");
            printf("  quit\n");
            continue;
        }

        if (strncmp(line, "tick", 4) == 0) {
            uint64_t count = 1;
            sscanf(line + 4, "%lu", (unsigned long *)&count);
            st = srr_pulse(conn, 1, count);
            printf("Sent pulse (clock tick x%lu): %s\n", (unsigned long)count, srr_status_str(st));
            continue;
        }

        send_cmd(conn, line);
    }

    srr_disconnect(conn);
    printf("Disconnected.\n");
    return 0;
}
