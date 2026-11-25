#ifndef SRR_UDP_H
#define SRR_UDP_H

#include "srr_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR UDP Datagram Transport
 * ============================================================================
 * Enables synchronous Send/Receive/Reply and pulses over local Wi-Fi / Ethernet
 * networks without connection setup overhead, head-of-line blocking, or TLS bloat.
 * Ideal for ESP32, Pico W, robotics, and real-time audio LAN control planes.
 */

#define SRR_UDP_DEFAULT_PORT 8888
#define SRR_UDP_MAX_MTU      1472 /* Standard Ethernet MTU minus IP/UDP headers */

typedef struct srr_udp_endpoint srr_udp_endpoint_t;

typedef struct {
    uint16_t port;              /* Port to bind (for server) or 0 (ephemeral for client) */
    uint32_t default_timeout_ms;/* Default timeout for synchronous operations */
    bool     enable_broadcast;  /* Enable SO_BROADCAST for LAN discovery */
} srr_udp_config_t;

/* Get default UDP configuration */
void srr_udp_default_config(srr_udp_config_t *cfg);

/*
 * Create a UDP client endpoint targeted to a remote peer (host:port).
 * host can be an IPv4 address (e.g. "192.168.1.100", "127.0.0.1", or "255.255.255.255").
 */
srr_udp_endpoint_t* srr_udp_client_create(const char *remote_host, uint16_t remote_port, const srr_udp_config_t *config);

/*
 * Create a UDP server endpoint bound to a specific local port (e.g. 8888).
 */
srr_udp_endpoint_t* srr_udp_server_create(uint16_t local_port, const srr_udp_config_t *config);

/* Close endpoint and release socket */
void srr_udp_close(srr_udp_endpoint_t *ep);

/*
 * Synchronous client Request/Reply transaction over UDP:
 * Transmits REQUEST datagram and waits for matching trans_id in REPLY or ERROR.
 * If timeout expires without reply, returns -1.
 */
int srr_udp_request(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms
);

/*
 * Fire-and-forget asynchronous pulse over UDP.
 */
int srr_udp_pulse(
    srr_udp_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len
);

/*
 * Server: Receive next incoming request or pulse datagram.
 * Fills in sender identity for subsequent reply.
 */
typedef struct srr_udp_peer srr_udp_peer_t;

int srr_udp_server_receive(
    srr_udp_endpoint_t *ep,
    srr_wire_header_t *out_hdr,
    uint8_t *payload_buf,
    size_t payload_cap,
    size_t *actual_payload_len,
    srr_udp_peer_t **out_peer,
    uint32_t timeout_ms
);

/*
 * Server: Reply to a received request using the captured peer handle.
 */
int srr_udp_server_reply(
    srr_udp_endpoint_t *ep,
    const srr_udp_peer_t *peer,
    uint64_t trans_id,
    uint16_t service_id,
    uint16_t operation_id,
    const void *reply_payload,
    size_t reply_len,
    bool is_error
);

/* Free peer handle */
void srr_udp_peer_free(srr_udp_peer_t *peer);

#ifdef __cplusplus
}
#endif

#endif /* SRR_UDP_H */
