#ifndef SRR_SERIAL_H
#define SRR_SERIAL_H

#include "srr_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR Serial Transport (POSIX Linux & macOS UART / pty)
 * ============================================================================
 * Provides byte transport, frame synchronization, and request/reply multiplexing
 * over physical or virtual serial lines.
 */

typedef struct srr_serial_port srr_serial_port_t;

typedef struct {
    uint32_t baud_rate;        /* Baud rate: 9600, 19200, 38400, 57600, 115200, 230400, etc. */
    uint32_t read_timeout_ms;  /* Timeout for frame reads (ms) */
    size_t   rx_buf_size;      /* Buffer size for raw COBS bytes (e.g. 2048) */
    size_t   decode_buf_size;  /* Buffer size for decoded frames (e.g. 2048) */
    size_t   tx_buf_size;      /* Buffer size for outgoing frames (e.g. 2048) */
} srr_serial_config_t;

/* Get default serial configuration */
void srr_serial_default_config(srr_serial_config_t *cfg);

/*
 * Open a serial device (e.g. "/dev/ttyUSB0", "/dev/ttyACM0", or virtual pty).
 * Configures termios into raw 8N1 mode.
 * Returns NULL on failure.
 */
srr_serial_port_t* srr_serial_open(const char *device_path, const srr_serial_config_t *config);

/*
 * Wrap an already open file descriptor (e.g. openpty master/slave or pipe)
 * as an SRR serial port for testing or embedded simulation.
 */
srr_serial_port_t* srr_serial_from_fd(int fd, bool owns_fd, const srr_serial_config_t *config);

/* Close serial port and free allocated resources */
void srr_serial_close(srr_serial_port_t *port);

/* Get underlying file descriptor */
int srr_serial_get_fd(const srr_serial_port_t *port);

/* Send an encoded SRR wire frame over serial */
int srr_serial_send_frame(
    srr_serial_port_t *port,
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len
);

/*
 * Read next valid frame from serial link with timeout.
 * Handles partial reads, COBS framing, and CRC verification.
 * Returns 0 on success, negative error code on timeout/link error:
 *   -1: Timeout
 *   -2: Link disconnected / EOF
 *   -3: Malformed frame / CRC error (non-fatal, can retry)
 */
int srr_serial_read_frame(
    srr_serial_port_t *port,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload,
    uint32_t timeout_ms
);

/*
 * Synchronous Request/Reply transaction over serial.
 * Transmits REQUEST frame and waits for matching trans_id in REPLY or ERROR.
 * Returns 0 on success, or negative error code:
 *   -1: Timeout
 *   -2: Link disconnected
 *   -3: Target returned ERROR frame (out_hdr->msg_type == SRR_WIRE_ERROR)
 */
int srr_serial_request(
    srr_serial_port_t *port,
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
 * Send a one-way asynchronous pulse over serial link.
 */
int srr_serial_pulse(
    srr_serial_port_t *port,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len
);

#ifdef __cplusplus
}
#endif

#endif /* SRR_SERIAL_H */
