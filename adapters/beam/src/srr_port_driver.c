#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "srr/srr.h"

/*
 * BEAM Port Driver for SRR
 * Communicates with Erlang/Elixir ports over stdin/stdout with 4-byte length prefixes.
 * Erlang invocation:
 *   Port = open_port({spawn_executable, "adapters/beam/srr_port_driver"}, [{packet, 4}, binary])
 */

static int read_exact(uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(STDIN_FILENO, buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int write_packet(uint8_t status_tag, const void *payload, size_t payload_len) {
    uint32_t total_len = 1 + (uint32_t)payload_len;
    uint32_t net_len = htonl(total_len);

    if (write(STDOUT_FILENO, &net_len, 4) != 4) return -1;
    if (write(STDOUT_FILENO, &status_tag, 1) != 1) return -1;
    if (payload_len > 0 && payload) {
        if (write(STDOUT_FILENO, payload, payload_len) != (ssize_t)payload_len) return -1;
    }
    return 0;
}

int main(void) {
    srr_connection_t *conn = NULL;

    while (1) {
        uint32_t net_len = 0;
        if (read_exact((uint8_t *)&net_len, 4) != 0) {
            break; /* Parent Erlang process closed port */
        }

        uint32_t len = ntohl(net_len);
        if (len == 0) continue;

        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf) break;

        if (read_exact(buf, len) != 0) {
            free(buf);
            break;
        }

        uint8_t cmd_tag = buf[0];
        uint8_t *args = buf + 1;
        size_t args_len = len - 1;

        switch (cmd_tag) {
            case 1: { /* CONNECT <service_name> */
                char svc[128] = {0};
                size_t cplen = args_len < sizeof(svc) - 1 ? args_len : sizeof(svc) - 1;
                memcpy(svc, args, cplen);

                if (conn) srr_disconnect(conn);
                srr_status_t st = srr_connect(svc, NULL, &conn);
                if (st == SRR_OK) {
                    write_packet(0, "OK", 2);
                } else {
                    const char *err = srr_status_str(st);
                    write_packet(1, err, strlen(err));
                }
                break;
            }

            case 2: { /* SEND <payload> */
                if (!conn) {
                    write_packet(1, "NOT_CONNECTED", 13);
                    break;
                }

                char reply[4096] = {0};
                size_t reply_sz = 0;
                srr_status_t st = srr_send(conn, args, args_len, reply, sizeof(reply), &reply_sz);
                if (st == SRR_OK) {
                    write_packet(0, reply, reply_sz > 0 ? reply_sz - 1 : 0);
                } else {
                    const char *err = srr_status_str(st);
                    write_packet(1, err, strlen(err));
                }
                break;
            }

            case 3: { /* PULSE <code:uint32> <val:uint64> */
                if (!conn) {
                    write_packet(1, "NOT_CONNECTED", 13);
                    break;
                }
                if (args_len < 12) {
                    write_packet(1, "INVALID_ARGS", 12);
                    break;
                }
                uint32_t code = 0;
                uint64_t val = 0;
                memcpy(&code, args, 4);
                memcpy(&val, args + 4, 8);
                srr_status_t st = srr_pulse(conn, code, val);
                if (st == SRR_OK) {
                    write_packet(0, "OK", 2);
                } else {
                    const char *err = srr_status_str(st);
                    write_packet(1, err, strlen(err));
                }
                break;
            }

            case 4: { /* DISCONNECT */
                if (conn) {
                    srr_disconnect(conn);
                    conn = NULL;
                }
                write_packet(0, "OK", 2);
                break;
            }

            default:
                write_packet(1, "UNKNOWN_COMMAND", 15);
                break;
        }

        free(buf);
    }

    if (conn) {
        srr_disconnect(conn);
    }
    return 0;
}
