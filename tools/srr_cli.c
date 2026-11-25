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
#include "srr_udp.h"
#include "srr_resolver.h"

static void print_usage(const char *prog) {
    printf("SRR Command-Line Tool\n\n");
    printf("Usage:\n");
    printf("  %s [target] <subcommand> [args...]\n\n", prog);
    printf("Target Options (select one for direct transport commands):\n");
    printf("  --ipc <name>             Target IPC service (Unix domain socket)\n");
    printf("  --serial <dev>           Target serial device (e.g. /dev/ttyACM0, /dev/ttyUSB0)\n");
    printf("  --baud <rate>            Serial baud rate (default: 115200)\n");
    printf("  --udp <host:port>        Target remote UDP endpoint (e.g. 192.168.1.50:8888)\n");
    printf("  --sim                    Target built-in simulated Pico endpoint\n\n");
    printf("Subcommands:\n");
    printf("  info                     Query device.info (Service 1, Op 1)\n");
    printf("  ping                     Send a PING request and measure round-trip latency\n");
    printf("  req <svc> <op> [payload] Send synchronous request with optional payload\n");
    printf("  pulse <svc> <op> [data]  Send asynchronous pulse\n");
    printf("  listen [seconds]         Listen for incoming pulses/events\n");
    printf("  resolve <name>           Query resolver for service endpoint recipe\n");
    printf("  register <name> <transport> <target> [baud] [svc_id] [ttl] Register service with resolver\n");
    printf("  unregister <name>        Remove service from resolver\n");
    printf("  list-services            List all registered services from resolver\n");
    printf("  serve --exec <cmd>       Run an SRR server executing script/command per request\n");
    printf("  help                     Display this help reference\n\n");
    printf("Examples:\n");
    printf("  %s --ipc my_service serve --exec ./my_handler.sh\n", prog);
    printf("  %s resolve temp-sensor\n", prog);
    printf("  %s register temp-sensor SERIAL /dev/ttyACM0 115200 4097 0\n", prog);
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
    const char *udp_target = NULL;
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
        } else if (strcmp(argv[cmd_idx], "--udp") == 0 && cmd_idx + 1 < argc) {
            udp_target = argv[cmd_idx + 1];
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

    /* Resolver subcommands (operate against local resolver service or default) */
    if (strcmp(subcmd, "resolve") == 0) {
        if (cmd_idx + 1 >= argc) {
            fprintf(stderr, "Error: usage: %s [--ipc <resolver>] resolve <service_name>\n", argv[0]);
            return 1;
        }
        const char *target_svc = argv[cmd_idx + 1];
        srr_endpoint_entry_t entry;
        srr_status_t st = srr_resolve(ipc_name, target_svc, &entry, 2000);
        if (st == SRR_OK) {
            printf("Resolved Service: '%s'\n", entry.service_name);
            printf("  Transport:   %s\n", srr_transport_to_string(entry.transport));
            printf("  Endpoint:    %s\n", entry.endpoint);
            if (entry.transport == SRR_TRANSPORT_SERIAL) {
                printf("  Baud Rate:   %u\n", entry.baud_rate);
            }
            if (entry.service_id > 0) {
                printf("  Service ID:  %u (0x%04X)\n", entry.service_id, entry.service_id);
            }
            if (entry.ttl_seconds > 0) {
                printf("  TTL:         %u seconds\n", entry.ttl_seconds);
            } else {
                printf("  TTL:         static (permanent)\n");
            }
            return 0;
        } else {
            fprintf(stderr, "Error: Service '%s' not found or resolver unreachable (%s)\n", target_svc, srr_status_str(st));
            return 1;
        }
    }

    if (strcmp(subcmd, "register") == 0) {
        if (cmd_idx + 3 >= argc) {
            fprintf(stderr, "Error: usage: %s register <name> <transport> <endpoint> [baud] [service_id] [ttl]\n", argv[0]);
            return 1;
        }
        srr_endpoint_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        strncpy(entry.service_name, argv[cmd_idx + 1], sizeof(entry.service_name) - 1);
        entry.transport = srr_transport_from_string(argv[cmd_idx + 2]);
        strncpy(entry.endpoint, argv[cmd_idx + 3], sizeof(entry.endpoint) - 1);
        if (cmd_idx + 4 < argc) entry.baud_rate = (uint32_t)atoi(argv[cmd_idx + 4]);
        if (cmd_idx + 5 < argc) entry.service_id = (uint16_t)atoi(argv[cmd_idx + 5]);
        if (cmd_idx + 6 < argc) entry.ttl_seconds = (uint32_t)atoi(argv[cmd_idx + 6]);

        srr_status_t st = srr_register_service(ipc_name, &entry, 2000);
        if (st == SRR_OK) {
            printf("Successfully registered '%s' (%s -> %s)\n", entry.service_name, srr_transport_to_string(entry.transport), entry.endpoint);
            return 0;
        } else {
            fprintf(stderr, "Error: Failed to register service with resolver (%s)\n", srr_status_str(st));
            return 1;
        }
    }

    if (strcmp(subcmd, "unregister") == 0) {
        if (cmd_idx + 1 >= argc) {
            fprintf(stderr, "Error: usage: %s unregister <service_name>\n", argv[0]);
            return 1;
        }
        srr_status_t st = srr_unregister_service(ipc_name, argv[cmd_idx + 1], 2000);
        if (st == SRR_OK) {
            printf("Successfully unregistered '%s'\n", argv[cmd_idx + 1]);
            return 0;
        } else {
            fprintf(stderr, "Error: Failed to unregister service (%s)\n", srr_status_str(st));
            return 1;
        }
    }

    if (strcmp(subcmd, "list-services") == 0) {
        srr_connection_t *conn = NULL;
        const char *svc = ipc_name ? ipc_name : SRR_RESOLVER_DEFAULT_SERVICE;
        srr_status_t st = srr_connect(svc, NULL, &conn);
        if (st != SRR_OK) {
            fprintf(stderr, "Error connecting to resolver '%s': %s\n", svc, srr_status_str(st));
            return 1;
        }
        char rep[4096] = {0};
        size_t rep_len = 0;
        st = srr_send_timed(conn, "LIST", 5, rep, sizeof(rep) - 1, &rep_len, 2000);
        srr_disconnect(conn);
        if (st == SRR_OK) {
            printf("%s", rep);
            return 0;
        } else {
            fprintf(stderr, "Error listing services: %s\n", srr_status_str(st));
            return 1;
        }
    }

    /* Serve subcommand: runs an SRR server and delegates request handling to a script */
    if (strcmp(subcmd, "serve") == 0) {
        const char *exec_cmd = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
                exec_cmd = argv[++i];
            }
        }
        if (!exec_cmd) {
            fprintf(stderr, "Error: 'serve' requires --exec <command_or_script>\n");
            fprintf(stderr, "Usage: %s --ipc <name> serve --exec <script>\n", argv[0]);
            fprintf(stderr, "       %s --udp <port> serve --exec <script>\n", argv[0]);
            return 1;
        }

        if (ipc_name) {
            srr_channel_t *chan = NULL;
            srr_status_t st = srr_channel_create_service(ipc_name, NULL, &chan);
            if (st != SRR_OK) {
                fprintf(stderr, "Error: Failed to create IPC service '%s': %s\n", ipc_name, srr_status_str(st));
                return 1;
            }
            printf("SRR Service listening on IPC '%s' (exec: %s)\n", ipc_name, exec_cmd);
            fflush(stdout);
            while (1) {
                srr_request_t *req = NULL;
                st = srr_receive(chan, &req);
                if (st != SRR_OK || !req) break;

                if (srr_request_is_pulse(req)) {
                    srr_request_dispose(req);
                    continue;
                }

                const char *req_data = (const char *)srr_request_data(req);
                size_t req_sz = srr_request_size(req);

                int in_pipe[2], out_pipe[2];
                if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
                    srr_reply(req, "ERR pipe_failed", 16);
                    continue;
                }

                pid_t pid = fork();
                if (pid == 0) {
                    close(in_pipe[1]);
                    close(out_pipe[0]);
                    dup2(in_pipe[0], STDIN_FILENO);
                    dup2(out_pipe[1], STDOUT_FILENO);
                    close(in_pipe[0]);
                    close(out_pipe[1]);
                    execl("/bin/sh", "sh", "-c", exec_cmd, (char *)NULL);
                    _exit(127);
                }

                close(in_pipe[0]);
                close(out_pipe[1]);
                if (req_sz > 0 && req_data) {
                    ssize_t w = write(in_pipe[1], req_data, req_sz);
                    (void)w;
                }
                close(in_pipe[1]);

                char reply_buf[8192] = {0};
                ssize_t n = read(out_pipe[0], reply_buf, sizeof(reply_buf) - 1);
                close(out_pipe[0]);
                waitpid(pid, NULL, 0);

                if (n < 0) n = 0;
                reply_buf[n] = '\0';
                if (n > 0 && reply_buf[n - 1] == '\n') {
                    reply_buf[--n] = '\0';
                }

                srr_reply(req, reply_buf, (size_t)n + 1);
            }
            srr_channel_destroy(chan);
            return 0;
        } else if (udp_target) {
            uint16_t port = (uint16_t)atoi(udp_target);
            if (port == 0) port = SRR_UDP_DEFAULT_PORT;
            srr_udp_config_t cfg;
            srr_udp_default_config(&cfg);
            srr_udp_endpoint_t *server = srr_udp_server_create(port, &cfg);
            if (!server) {
                fprintf(stderr, "Error: Could not bind UDP server on port %u\n", port);
                return 1;
            }
            printf("SRR Service listening on UDP port %u (exec: %s)\n", port, exec_cmd);
            fflush(stdout);
            while (1) {
                srr_wire_header_t hdr;
                uint8_t payload[4096] = {0};
                size_t actual_len = 0;
                srr_udp_peer_t *peer = NULL;
                int rc = srr_udp_server_receive(server, &hdr, payload, sizeof(payload) - 1, &actual_len, &peer, 1000);
                if (rc == 0 && peer) {
                    int in_pipe[2], out_pipe[2];
                    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
                        srr_udp_peer_free(peer);
                        continue;
                    }
                    pid_t pid = fork();
                    if (pid == 0) {
                        close(in_pipe[1]); close(out_pipe[0]);
                        dup2(in_pipe[0], STDIN_FILENO);
                        dup2(out_pipe[1], STDOUT_FILENO);
                        close(in_pipe[0]); close(out_pipe[1]);
                        execl("/bin/sh", "sh", "-c", exec_cmd, (char *)NULL);
                        _exit(127);
                    }
                    close(in_pipe[0]); close(out_pipe[1]);
                    if (actual_len > 0) {
                        ssize_t w = write(in_pipe[1], payload, actual_len);
                        (void)w;
                    }
                    close(in_pipe[1]);

                    char reply_buf[8192] = {0};
                    ssize_t n = read(out_pipe[0], reply_buf, sizeof(reply_buf) - 1);
                    close(out_pipe[0]);
                    waitpid(pid, NULL, 0);

                    if (n < 0) n = 0;
                    reply_buf[n] = '\0';
                    if (n > 0 && reply_buf[n - 1] == '\n') reply_buf[--n] = '\0';

                    srr_udp_server_reply(server, peer, hdr.trans_id, hdr.service_id, hdr.operation_id,
                                         reply_buf, (size_t)n + 1, false);
                    srr_udp_peer_free(peer);
                }
            }
            srr_udp_close(server);
            return 0;
        } else {
            fprintf(stderr, "Error: 'serve' requires either --ipc <name> or --udp <port>\n");
            return 1;
        }
    }

    pid_t sim_pid = 0;
    srr_connection_t *ipc_conn = NULL;
    srr_serial_port_t *serial_port = NULL;
    srr_udp_endpoint_t *udp_ep = NULL;

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
    } else if (udp_target) {
        char host[128] = {0};
        uint16_t port = SRR_UDP_DEFAULT_PORT;
        char *colon = strchr(udp_target, ':');
        if (colon) {
            size_t host_len = colon - udp_target;
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, udp_target, host_len);
            port = (uint16_t)atoi(colon + 1);
        } else {
            strncpy(host, udp_target, sizeof(host) - 1);
        }
        srr_udp_config_t ucfg;
        srr_udp_default_config(&ucfg);
        udp_ep = srr_udp_client_create(host, port, &ucfg);
        if (!udp_ep) {
            fprintf(stderr, "Error: Could not initialize UDP client for %s:%u\n", host, port);
            return 1;
        }
    } else if (ipc_name) {
        srr_status_t st = srr_connect(ipc_name, NULL, &ipc_conn);
        if (st != SRR_OK) {
            fprintf(stderr, "Error: Could not connect to IPC service '%s': %s\n", ipc_name, srr_status_str(st));
            return 1;
        }
    } else {
        fprintf(stderr, "Error: No target specified. Use --ipc <name>, --serial <dev>, --udp <host:port>, or --sim\n\n");
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
        } else if (udp_ep) {
            int rc = srr_udp_request(udp_ep, 1, 1, NULL, 0, reply, sizeof(reply) - 1, &actual_len, 2000);
            if (rc == 0) {
                reply[actual_len] = '\0';
                printf("UDP Device Info: %s\n", reply);
            } else {
                fprintf(stderr, "Error querying UDP info: code %d\n", rc);
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
        } else if (udp_ep) {
            int rc = srr_udp_request(udp_ep, 1, 1, "PING", 4, reply, sizeof(reply) - 1, &actual_len, 2000);
            uint64_t t1 = get_time_ns();
            if (rc == 0) {
                printf("PONG received via UDP in %.2f µs (Reply: %.*s)\n", (double)(t1 - t0) / 1000.0, (int)actual_len, reply);
            } else {
                fprintf(stderr, "UDP Ping failed: code %d\n", rc);
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
            } else if (udp_ep) {
                int rc = srr_udp_request(udp_ep, svc, op, payload_str, payload_len, reply, sizeof(reply) - 1, &actual_len, 2000);
                if (rc == 0) {
                    reply[actual_len] = '\0';
                    printf("[UDP REPLY (%zu bytes)]: %s\n", actual_len, reply);
                } else if (rc == -3) {
                    printf("[ERROR]: UDP Endpoint returned error frame for Service %u, Op %u\n", svc, op);
                    exit_code = 3;
                } else {
                    fprintf(stderr, "UDP Request failed: code %d\n", rc);
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
            } else if (udp_ep) {
                int rc = srr_udp_pulse(udp_ep, svc, op, data_str, strlen(data_str));
                if (rc == 0) {
                    printf("[OK] UDP Pulse transmitted successfully (Service %u, Op %u)\n", svc, op);
                } else {
                    fprintf(stderr, "Failed to send UDP pulse: code %d\n", rc);
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
    if (udp_ep) srr_udp_close(udp_ep);
    if (ipc_conn) srr_disconnect(ipc_conn);
    if (sim_pid > 0) {
        kill(sim_pid, SIGTERM);
        waitpid(sim_pid, NULL, 0);
    }

    return exit_code;
}
