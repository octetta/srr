#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "srr_udp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct srr_udp_peer {
    struct sockaddr_in addr;
    socklen_t addr_len;
};

struct srr_udp_endpoint {
    int fd;
    bool is_server;
    srr_udp_config_t config;

    struct sockaddr_in peer_addr; /* Default remote address for client */
    socklen_t peer_addr_len;

    uint32_t session_id;
    uint64_t next_trans_id;

    uint8_t tx_scratch[SRR_UDP_MAX_MTU + 64];
    uint8_t rx_scratch[SRR_UDP_MAX_MTU + 64];
};

void srr_udp_default_config(srr_udp_config_t *cfg) {
    if (!cfg) return;
    cfg->port = 0;
    cfg->default_timeout_ms = 1000;
    cfg->enable_broadcast = false;
}

srr_udp_endpoint_t* srr_udp_client_create(const char *remote_host, uint16_t remote_port, const srr_udp_config_t *config) {
    if (!remote_host || remote_port == 0) return NULL;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    srr_udp_endpoint_t *ep = (srr_udp_endpoint_t *)calloc(1, sizeof(srr_udp_endpoint_t));
    if (!ep) {
        close(fd);
        return NULL;
    }

    if (config) ep->config = *config;
    else srr_udp_default_config(&ep->config);

    if (ep->config.enable_broadcast) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }

    ep->fd = fd;
    ep->is_server = false;
    ep->session_id = (uint32_t)time(NULL);
    ep->next_trans_id = 1;

    memset(&ep->peer_addr, 0, sizeof(ep->peer_addr));
    ep->peer_addr.sin_family = AF_INET;
    ep->peer_addr.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_host, &ep->peer_addr.sin_addr) <= 0) {
        srr_udp_close(ep);
        return NULL;
    }
    ep->peer_addr_len = sizeof(ep->peer_addr);

    return ep;
}

srr_udp_endpoint_t* srr_udp_server_create(uint16_t local_port, const srr_udp_config_t *config) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(local_port);

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(fd);
        return NULL;
    }

    srr_udp_endpoint_t *ep = (srr_udp_endpoint_t *)calloc(1, sizeof(srr_udp_endpoint_t));
    if (!ep) {
        close(fd);
        return NULL;
    }

    if (config) ep->config = *config;
    else srr_udp_default_config(&ep->config);

    ep->fd = fd;
    ep->is_server = true;
    ep->session_id = (uint32_t)time(NULL);
    ep->next_trans_id = 1;

    return ep;
}

void srr_udp_close(srr_udp_endpoint_t *ep) {
    if (!ep) return;
    if (ep->fd >= 0) {
        close(ep->fd);
        ep->fd = -1;
    }
    free(ep);
}

static size_t pack_udp_datagram(
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len,
    uint8_t *out_buf,
    size_t out_cap)
{
    /* UDP datagram layout: [Header (32B LE)] + [Payload (len)] + [CRC16 (2B LE)] */
    if (SRR_HEADER_SIZE + payload_len + SRR_CRC_SIZE > out_cap) return 0;

    size_t hdr_bytes = srr_wire_encode_header(hdr, out_buf, out_cap);
    if (hdr_bytes != SRR_HEADER_SIZE) return 0;

    if (payload && payload_len > 0) {
        memcpy(out_buf + SRR_HEADER_SIZE, payload, payload_len);
    }

    /* Compute CRC over header + payload */
    uint16_t crc = srr_crc16(out_buf, SRR_HEADER_SIZE + payload_len, 0xFFFF);
    out_buf[SRR_HEADER_SIZE + payload_len] = (uint8_t)(crc & 0xFF);
    out_buf[SRR_HEADER_SIZE + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return SRR_HEADER_SIZE + payload_len + SRR_CRC_SIZE;
}

static bool unpack_udp_datagram(
    const uint8_t *dgram,
    size_t dgram_len,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload)
{
    if (dgram_len < SRR_MIN_FRAME_RAW) return false;

    /* Verify CRC16 */
    uint16_t expected_crc = (uint16_t)dgram[dgram_len - 2] | ((uint16_t)dgram[dgram_len - 1] << 8);
    uint16_t calc_crc = srr_crc16(dgram, dgram_len - 2, 0xFFFF);
    if (calc_crc != expected_crc) return false;

    if (!srr_wire_decode_header(dgram, SRR_HEADER_SIZE, out_hdr)) return false;

    if (out_hdr->magic != SRR_WIRE_MAGIC || out_hdr->version != SRR_WIRE_VERSION) return false;

    size_t expected_payload_len = dgram_len - SRR_HEADER_SIZE - SRR_CRC_SIZE;
    if (out_hdr->payload_len != expected_payload_len) return false;

    if (out_payload) {
        *out_payload = (expected_payload_len > 0) ? (dgram + SRR_HEADER_SIZE) : NULL;
    }
    return true;
}

int srr_udp_request(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms)
{
    if (!ep || ep->fd < 0) return -1;

    uint64_t tid = ep->next_trans_id++;

    srr_wire_header_t req_hdr;
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.magic = SRR_WIRE_MAGIC;
    req_hdr.version = SRR_WIRE_VERSION;
    req_hdr.msg_type = SRR_WIRE_REQUEST;
    req_hdr.service_id = service_id;
    req_hdr.operation_id = operation_id;
    req_hdr.session_id = ep->session_id;
    req_hdr.trans_id = tid;
    req_hdr.payload_len = (uint32_t)req_len;

    size_t dgram_len = pack_udp_datagram(&req_hdr, req_payload, req_len, ep->tx_scratch, sizeof(ep->tx_scratch));
    if (dgram_len == 0) return -1;

    ssize_t sent = sendto(ep->fd, ep->tx_scratch, dgram_len, 0, (struct sockaddr *)&ep->peer_addr, ep->peer_addr_len);
    if (sent != (ssize_t)dgram_len) return -1;

    struct timespec start_ts, now_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        int64_t elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000 +
                             (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return -1; /* Timeout */
        }

        int wait_ms = (int)(timeout_ms - elapsed_ms);
        struct pollfd pfd = { .fd = ep->fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, wait_ms);
        if (ret <= 0) return -1; /* Timeout or error */

        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(ep->fd, ep->rx_scratch, sizeof(ep->rx_scratch), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) continue;

        srr_wire_header_t rep_hdr;
        const uint8_t *rep_payload = NULL;
        if (!unpack_udp_datagram(ep->rx_scratch, (size_t)n, &rep_hdr, &rep_payload)) {
            continue; /* Ignore malformed datagram or bad CRC */
        }

        if (rep_hdr.trans_id == tid) {
            if (rep_hdr.msg_type == SRR_WIRE_REPLY) {
                size_t cpy = rep_hdr.payload_len < reply_cap ? rep_hdr.payload_len : reply_cap;
                if (reply_buf && cpy > 0 && rep_payload) {
                    memcpy(reply_buf, rep_payload, cpy);
                }
                if (actual_reply_len) *actual_reply_len = rep_hdr.payload_len;
                return 0; /* Success */
            } else if (rep_hdr.msg_type == SRR_WIRE_ERROR) {
                return -3; /* Error */
            }
        }
    }
}

int srr_udp_pulse(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len)
{
    if (!ep || ep->fd < 0) return -1;

    srr_wire_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SRR_WIRE_MAGIC;
    hdr.version = SRR_WIRE_VERSION;
    hdr.msg_type = SRR_WIRE_PULSE;
    hdr.service_id = service_id;
    hdr.operation_id = operation_id;
    hdr.session_id = ep->session_id;
    hdr.trans_id = 0;
    hdr.payload_len = (uint32_t)payload_len;

    size_t dgram_len = pack_udp_datagram(&hdr, payload, payload_len, ep->tx_scratch, sizeof(ep->tx_scratch));
    if (dgram_len == 0) return -1;

    ssize_t sent = sendto(ep->fd, ep->tx_scratch, dgram_len, 0, (struct sockaddr *)&ep->peer_addr, ep->peer_addr_len);
    return (sent == (ssize_t)dgram_len) ? 0 : -1;
}

int srr_udp_server_receive(
    srr_udp_endpoint_t *ep,
    srr_wire_header_t *out_hdr,
    uint8_t *payload_buf,
    size_t payload_cap,
    size_t *actual_payload_len,
    srr_udp_peer_t **out_peer,
    uint32_t timeout_ms)
{
    if (!ep || ep->fd < 0 || !out_hdr) return -1;

    struct pollfd pfd = { .fd = ep->fd, .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, (int)timeout_ms);
    if (ret <= 0) return -1; /* Timeout or error */

    srr_udp_peer_t *peer = (srr_udp_peer_t *)calloc(1, sizeof(srr_udp_peer_t));
    if (!peer) return -1;

    peer->addr_len = sizeof(peer->addr);
    ssize_t n = recvfrom(ep->fd, ep->rx_scratch, sizeof(ep->rx_scratch), 0, (struct sockaddr *)&peer->addr, &peer->addr_len);
    if (n <= 0) {
        free(peer);
        return -1;
    }

    const uint8_t *dgram_payload = NULL;
    if (!unpack_udp_datagram(ep->rx_scratch, (size_t)n, out_hdr, &dgram_payload)) {
        free(peer);
        return -2; /* CRC / framing corruption */
    }

    size_t cpy = out_hdr->payload_len < payload_cap ? out_hdr->payload_len : payload_cap;
    if (payload_buf && cpy > 0 && dgram_payload) {
        memcpy(payload_buf, dgram_payload, cpy);
    }
    if (actual_payload_len) *actual_payload_len = out_hdr->payload_len;

    if (out_peer) *out_peer = peer;
    else free(peer);

    return 0;
}

int srr_udp_server_reply(
    srr_udp_endpoint_t *ep,
    const srr_udp_peer_t *peer,
    uint64_t trans_id,
    uint16_t service_id,
    uint16_t operation_id,
    const void *reply_payload,
    size_t reply_len,
    bool is_error)
{
    if (!ep || ep->fd < 0 || !peer) return -1;

    srr_wire_header_t rep_hdr;
    memset(&rep_hdr, 0, sizeof(rep_hdr));
    rep_hdr.magic = SRR_WIRE_MAGIC;
    rep_hdr.version = SRR_WIRE_VERSION;
    rep_hdr.msg_type = is_error ? SRR_WIRE_ERROR : SRR_WIRE_REPLY;
    rep_hdr.service_id = service_id;
    rep_hdr.operation_id = operation_id;
    rep_hdr.session_id = ep->session_id;
    rep_hdr.trans_id = trans_id;
    rep_hdr.payload_len = is_error ? 0 : (uint32_t)reply_len;

    size_t dgram_len = pack_udp_datagram(&rep_hdr, reply_payload, rep_hdr.payload_len, ep->tx_scratch, sizeof(ep->tx_scratch));
    if (dgram_len == 0) return -1;

    ssize_t sent = sendto(ep->fd, ep->tx_scratch, dgram_len, 0, (struct sockaddr *)&peer->addr, peer->addr_len);
    return (sent == (ssize_t)dgram_len) ? 0 : -1;
}

void srr_udp_peer_free(srr_udp_peer_t *peer) {
    free(peer);
}
