#define _POSIX_C_SOURCE 200809L
#include "srr_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <time.h>

struct srr_serial_port {
    int fd;
    bool owns_fd;
    srr_serial_config_t config;

    uint32_t session_id;
    uint64_t next_trans_id;

    /* Buffers for parser and transmission */
    uint8_t *rx_buf;
    size_t   rx_cap;
    uint8_t *decode_buf;
    size_t   decode_cap;
    uint8_t *tx_buf;
    size_t   tx_cap;

    srr_wire_parser_t parser;
};

static speed_t get_termios_baud(uint32_t baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return B115200;
    }
}

void srr_serial_default_config(srr_serial_config_t *cfg) {
    if (!cfg) return;
    cfg->baud_rate = 115200;
    cfg->read_timeout_ms = 1000;
    cfg->rx_buf_size = 2048;
    cfg->decode_buf_size = 2048;
    cfg->tx_buf_size = 2048;
}

srr_serial_port_t* srr_serial_from_fd(int fd, bool owns_fd, const srr_serial_config_t *config) {
    if (fd < 0) return NULL;

    srr_serial_port_t *port = (srr_serial_port_t *)calloc(1, sizeof(srr_serial_port_t));
    if (!port) return NULL;

    if (config) {
        port->config = *config;
    } else {
        srr_serial_default_config(&port->config);
    }

    port->fd = fd;
    port->owns_fd = owns_fd;
    port->session_id = (uint32_t)time(NULL);
    port->next_trans_id = 1;

    /* Ensure fd is in non-blocking mode */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    port->rx_cap = port->config.rx_buf_size ? port->config.rx_buf_size : 2048;
    port->decode_cap = port->config.decode_buf_size ? port->config.decode_buf_size : 2048;
    port->tx_cap = port->config.tx_buf_size ? port->config.tx_buf_size : 2048;

    port->rx_buf = (uint8_t *)malloc(port->rx_cap);
    port->decode_buf = (uint8_t *)malloc(port->decode_cap);
    port->tx_buf = (uint8_t *)malloc(port->tx_cap);

    if (!port->rx_buf || !port->decode_buf || !port->tx_buf) {
        srr_serial_close(port);
        return NULL;
    }

    srr_wire_parser_init(&port->parser, port->rx_buf, port->rx_cap, port->decode_buf, port->decode_cap);
    return port;
}

srr_serial_port_t* srr_serial_open(const char *device_path, const srr_serial_config_t *config) {
    if (!device_path) return NULL;

    int fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return NULL;

    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        /* Standard POSIX raw 8N1 mode */
        tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        tio.c_oflag &= ~OPOST;
        tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tio.c_cflag &= ~(CSIZE | PARENB);
        tio.c_cflag |= CS8;

        speed_t speed = get_termios_baud(config ? config->baud_rate : 115200);
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);

        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif

        tcsetattr(fd, TCSANOW, &tio);
        tcflush(fd, TCIOFLUSH);
    }

    return srr_serial_from_fd(fd, true, config);
}

void srr_serial_close(srr_serial_port_t *port) {
    if (!port) return;
    if (port->owns_fd && port->fd >= 0) {
        close(port->fd);
        port->fd = -1;
    }
    free(port->rx_buf);
    free(port->decode_buf);
    free(port->tx_buf);
    free(port);
}

int srr_serial_get_fd(const srr_serial_port_t *port) {
    return port ? port->fd : -1;
}

int srr_serial_send_frame(
    srr_serial_port_t *port,
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len)
{
    if (!port || !hdr) return -1;

    size_t encoded_len = srr_wire_encode_frame(hdr, payload, payload_len, port->tx_buf, port->tx_cap);
    if (encoded_len == 0) return -1;

    size_t written = 0;
    while (written < encoded_len) {
        ssize_t n = write(port->fd, port->tx_buf + written, encoded_len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = port->fd, .events = POLLOUT, .revents = 0 };
                if (poll(&pfd, 1, 1000) <= 0) return -1;
                continue;
            }
            return -2; /* Link loss */
        }
        if (n == 0) return -2;
        written += (size_t)n;
    }
    return 0;
}

int srr_serial_read_frame(
    srr_serial_port_t *port,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload,
    uint32_t timeout_ms)
{
    if (!port || !out_hdr || !out_payload) return -1;

    struct timespec start_ts, now_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    uint8_t byte;
    while (1) {
        ssize_t n = read(port->fd, &byte, 1);
        if (n > 0) {
            srr_parser_status_t st = srr_wire_parser_feed_byte(&port->parser, byte, out_hdr, out_payload);
            if (st == SRR_PARSER_FRAME_READY) {
                return 0; /* Got complete, valid frame */
            } else if (st == SRR_PARSER_ERR_CRC || st == SRR_PARSER_ERR_MALFORMED) {
                /* Non-fatal framing error; parser auto-hunts for next delimiter */
                continue;
            } else if (st == SRR_PARSER_ERR_OVERFLOW) {
                srr_wire_parser_reset(&port->parser);
                continue;
            }
            /* SRR_PARSER_NEED_MORE: continue reading */
            continue;
        }

        if (n == 0) {
            /* EOF or link disconnect */
            return -2;
        }

        if (errno == EINTR) continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            int64_t elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000 +
                                 (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
            if (elapsed_ms >= (int64_t)timeout_ms) {
                return -1; /* Timeout */
            }

            int wait_ms = (int)(timeout_ms - elapsed_ms);
            struct pollfd pfd = { .fd = port->fd, .events = POLLIN, .revents = 0 };
            int poll_rc = poll(&pfd, 1, wait_ms);
            if (poll_rc < 0) {
                if (errno == EINTR) continue;
                return -2;
            }
            if (poll_rc == 0) {
                return -1; /* Timeout */
            }
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                return -2;
            }
            continue;
        }

        return -2; /* Other read error */
    }
}

int srr_serial_request(
    srr_serial_port_t *port,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms)
{
    if (!port) return -1;

    uint64_t tid = port->next_trans_id++;

    srr_wire_header_t req_hdr;
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.magic = SRR_WIRE_MAGIC;
    req_hdr.version = SRR_WIRE_VERSION;
    req_hdr.msg_type = SRR_WIRE_REQUEST;
    req_hdr.service_id = service_id;
    req_hdr.operation_id = operation_id;
    req_hdr.session_id = port->session_id;
    req_hdr.trans_id = tid;
    req_hdr.payload_len = (uint32_t)req_len;

    int send_rc = srr_serial_send_frame(port, &req_hdr, req_payload, req_len);
    if (send_rc != 0) return send_rc;

    struct timespec start_ts, now_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        int64_t elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000 +
                             (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return -1; /* Timeout */
        }

        uint32_t rem_ms = (uint32_t)(timeout_ms - elapsed_ms);
        srr_wire_header_t in_hdr;
        const uint8_t *in_payload = NULL;

        int read_rc = srr_serial_read_frame(port, &in_hdr, &in_payload, rem_ms);
        if (read_rc != 0) return read_rc;

        /* Filter by matching transaction ID */
        if (in_hdr.trans_id == tid) {
            if (in_hdr.msg_type == SRR_WIRE_REPLY) {
                size_t copy_len = in_hdr.payload_len < reply_cap ? in_hdr.payload_len : reply_cap;
                if (reply_buf && copy_len > 0 && in_payload) {
                    memcpy(reply_buf, in_payload, copy_len);
                }
                if (actual_reply_len) {
                    *actual_reply_len = in_hdr.payload_len;
                }
                return 0; /* Success! */
            } else if (in_hdr.msg_type == SRR_WIRE_ERROR) {
                return -3; /* Error reply */
            }
        }
        /* Mismatched transaction or pulse: continue waiting */
    }
}

int srr_serial_pulse(
    srr_serial_port_t *port,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len)
{
    if (!port) return -1;

    srr_wire_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SRR_WIRE_MAGIC;
    hdr.version = SRR_WIRE_VERSION;
    hdr.msg_type = SRR_WIRE_PULSE;
    hdr.service_id = service_id;
    hdr.operation_id = operation_id;
    hdr.session_id = port->session_id;
    hdr.trans_id = 0; /* Pulses do not correlate */
    hdr.payload_len = (uint32_t)payload_len;

    return srr_serial_send_frame(port, &hdr, payload, payload_len);
}
