#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "srr/srr.h"
#include "srr_serial.h"
#include "srr_wire.h"

static void print_usage(const char *prog) {
    printf("SRR Command-Line Tool\n\n");
    printf("Usage:\n");
    printf("  %s [target] <subcommand> [args...]\n\n", prog);
    printf("Target Options (select one):\n");
    printf("  --ipc <name>             Target IPC service (Unix domain socket)\n");
    printf("  --serial <dev>           Target serial device (e.g. /dev/ttyACM0, /dev/ttyUSB0)\n");
    printf("  --baud <rate>            Serial baud rate (default: 115200)\n");
    printf("  --sim                    Target built-in simulated Pico endpoint\n\n");
    printf("Subcommands:\n");
    printf("  info                     Query device.info (Service 1, Op 1)\n");
    printf("  ping                     Send a PING request and measure round-trip latency\n");
    printf("  req <svc> <op> [payload] Send synchronous request with optional payload\n");
    printf("  pulse <svc> <op> [data]  Send asynchronous pulse\n");
    printf("  listen [seconds]         Listen for incoming pulses/events\n");
    printf("  help                     Display this help reference\n\n");
    printf("Examples:\n");
    printf("  %s --sim info\n", prog);
    printf("  %s --ipc synth_engine req 1 1\n", prog);
    printf("  %s --serial /dev/ttyACM0 --baud 115200 info\n", prog);
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *ipc_name = NULL;
    const char *serial_dev = NULL;
    uint32_t baud_rate = 115200;
    bool is_sim = false;
    int cmd_idx = 1;

    /* Parse target options */
    while (cmd_idx < argc) {
        if (strcmp(argv[cmd_idx], "--ipc") == 0 && cmd_idx + 1 < argc) {
            ipc_name = argv[cmd_idx + 1];
            cmd_idx += 2;
        } else if (strcmp(argv[cmd_idx], "--serial") == 0 && cmd_idx + 1 < argc) {
            serial_dev = argv[cmd_idx + 1];
            cmd_idx += 2;
        } else if (strcmp(argv[cmd_idx], "--baud") == 0 && cmd_idx + 1 < argc) {
            baud_rate = (uint32_t)atoi(argv[cmd_idx + 1]);
            cmd_idx += 2;
        } else if (strcmp(argv[cmd_idx], "--sim") == 0) {
            is_sim = true;
            cmd_idx += 1;
        } else if (strcmp(argv[cmd_idx], "help") == 0 || strcmp(argv[cmd_idx], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            break; /* Subcommand starts */
        }
    }

    if (cmd_idx >= argc) {
        fprintf(stderr, "Error: No subcommand specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *subcmd = argv[cmd_idx];
    pid_t sim_pid = 0;
    srr_connection_t *ipc_conn = NULL;
    srr_serial_port_t *serial_port = NULL;

    /* Establish connection based on target */
    if (is_sim) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            perror("socketpair");
            return 1;
        }

        sim_pid = fork();
        if (sim_pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDIN_FILENO);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            execl("examples/pico/pico_device_sim", "pico_device_sim", (char *)NULL);
            perror("execl");
            exit(1);
        }

        close(fds[1]);
        serial_port = srr_serial_from_fd(fds[0], true, NULL);
        if (!serial_port) {
            fprintf(stderr, "Error: Failed to initialize serial adapter for simulation.\n");
            kill(sim_pid, SIGKILL);
            return 1;
        }
    } else if (serial_dev) {
        srr_serial_config_t cfg;
        srr_serial_default_config(&cfg);
        cfg.baud_rate = baud_rate;
        serial_port = srr_serial_open(serial_dev, &cfg);
        if (!serial_port) {
            fprintf(stderr, "Error: Could not open serial device %s (baud: %u)\n", serial_dev, baud_rate);
            return 1;
        }
    } else if (ipc_name) {
        srr_status_t st = srr_connect(ipc_name, NULL, &ipc_conn);
        if (st != SRR_OK) {
            fprintf(stderr, "Error: Could not connect to IPC service '%s': %s\n", ipc_name, srr_status_str(st));
            return 1;
        }
    } else {
        fprintf(stderr, "Error: No target specified. Use --ipc <name>, --serial <dev>, or --sim\n\n");
        print_usage(argv[0]);
        return 1;
    }

    int exit_code = 0;

    /* Subcommand handling */
    if (strcmp(subcmd, "info") == 0) {
        char reply[256] = {0};
        size_t actual_len = 0;

        if (serial_port) {
            int rc = srr_serial_request(serial_port, 1, 1, NULL, 0, reply, sizeof(reply) - 1, &actual_len, 2000);
            if (rc == 0) {
                reply[actual_len] = '\0';
                printf("Device Info: %s\n", reply);
            } else {
                fprintf(stderr, "Error querying info: code %d\n", rc);
                exit_code = 2;
            }
        } else if (ipc_conn) {
            srr_status_t st = srr_send(ipc_conn, "STATUS", 6, reply, sizeof(reply) - 1, &actual_len);
            if (st == SRR_OK) {
                reply[actual_len] = '\0';
                printf("IPC Service Info: %s\n", reply);
            } else {
                fprintf(stderr, "Error: %s\n", srr_status_str(st));
                exit_code = 2;
            }
        }
    } else if (strcmp(subcmd, "ping") == 0) {
        char reply[128] = {0};
        size_t actual_len = 0;
        uint64_t t0 = get_time_ns();

        if (serial_port) {
            int rc = srr_serial_request(serial_port, 1, 1, "PING", 4, reply, sizeof(reply) - 1, &actual_len, 2000);
            uint64_t t1 = get_time_ns();
            if (rc == 0) {
                printf("PONG received in %.2f µs (Payload: %.*s)\n", (double)(t1 - t0) / 1000.0, (int)actual_len, reply);
            } else {
                fprintf(stderr, "Ping failed: code %d\n", rc);
                exit_code = 2;
            }
        } else if (ipc_conn) {
            srr_status_t st = srr_send(ipc_conn, "PING", 4, reply, sizeof(reply) - 1, &actual_len);
            uint64_t t1 = get_time_ns();
            if (st == SRR_OK) {
                printf("PONG received in %.2f µs (Reply: %.*s)\n", (double)(t1 - t0) / 1000.0, (int)actual_len, reply);
            } else {
                fprintf(stderr, "Ping failed: %s\n", srr_status_str(st));
                exit_code = 2;
            }
        }
    } else if (strcmp(subcmd, "req") == 0) {
        if (cmd_idx + 2 >= argc) {
            fprintf(stderr, "Usage: %s [target] req <service_id> <operation_id> [payload]\n", argv[0]);
            exit_code = 1;
        } else {
            uint16_t svc = (uint16_t)atoi(argv[cmd_idx + 1]);
            uint16_t op = (uint16_t)atoi(argv[cmd_idx + 2]);
            const char *payload_str = (cmd_idx + 3 < argc) ? argv[cmd_idx + 3] : "";
            size_t payload_len = strlen(payload_str);

            char reply[1024] = {0};
            size_t actual_len = 0;

            if (serial_port) {
                int rc = srr_serial_request(serial_port, svc, op, payload_str, payload_len, reply, sizeof(reply) - 1, &actual_len, 2000);
                if (rc == 0) {
                    reply[actual_len] = '\0';
                    printf("[REPLY (%zu bytes)]: %s\n", actual_len, reply);
                } else if (rc == -3) {
                    printf("[ERROR]: Endpoint returned error frame for Service %u, Op %u\n", svc, op);
                    exit_code = 3;
                } else {
                    fprintf(stderr, "Request failed: code %d\n", rc);
                    exit_code = 2;
                }
            } else if (ipc_conn) {
                srr_status_t st = srr_send(ipc_conn, payload_str, payload_len, reply, sizeof(reply) - 1, &actual_len);
                if (st == SRR_OK) {
                    reply[actual_len] = '\0';
                    printf("[REPLY (%zu bytes)]: %s\n", actual_len, reply);
                } else {
                    fprintf(stderr, "Request failed: %s\n", srr_status_str(st));
                    exit_code = 2;
                }
            }
        }
    } else if (strcmp(subcmd, "pulse") == 0) {
        if (cmd_idx + 2 >= argc) {
            fprintf(stderr, "Usage: %s [target] pulse <service_id> <operation_id> [payload]\n", argv[0]);
            exit_code = 1;
        } else {
            uint16_t svc = (uint16_t)atoi(argv[cmd_idx + 1]);
            uint16_t op = (uint16_t)atoi(argv[cmd_idx + 2]);
            const char *data_str = (cmd_idx + 3 < argc) ? argv[cmd_idx + 3] : "";

            if (serial_port) {
                int rc = srr_serial_pulse(serial_port, svc, op, data_str, strlen(data_str));
                if (rc == 0) {
                    printf("[OK] Pulse transmitted successfully (Service %u, Op %u)\n", svc, op);
                } else {
                    fprintf(stderr, "Failed to send pulse: code %d\n", rc);
                    exit_code = 2;
                }
            } else if (ipc_conn) {
                srr_status_t st = srr_pulse(ipc_conn, svc, op);
                if (st == SRR_OK) {
                    printf("[OK] Pulse sent successfully (Code %u, Val %u)\n", svc, op);
                } else {
                    fprintf(stderr, "Failed to send pulse: %s\n", srr_status_str(st));
                    exit_code = 2;
                }
            }
        }
    } else {
        fprintf(stderr, "Unknown subcommand '%s'\n\n", subcmd);
        print_usage(argv[0]);
        exit_code = 1;
    }

    /* Cleanup */
    if (serial_port) srr_serial_close(serial_port);
    if (ipc_conn) srr_disconnect(ipc_conn);
    if (sim_pid > 0) {
        kill(sim_pid, SIGTERM);
        waitpid(sim_pid, NULL, 0);
    }

    return exit_code;
}
