#ifndef SRR_WIRE_H
#define SRR_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR Wire Protocol: Architecture-Independent Binary Framing
 * ============================================================================
 * Features:
 *   - COBS (Consistent Overhead Byte Stuffing) with 0x00 delimiter.
 *   - CRC-16-CCITT integrity verification.
 *   - Explicit Little-Endian field serialization (no packed structs).
 *   - Streaming chunk parser with zero heap allocation.
 */

#define SRR_WIRE_MAGIC   0x53525231U  /* "SRR1" */
#define SRR_WIRE_VERSION 1
#define SRR_WIRE_DELIM   0x00U

#define SRR_HEADER_SIZE  32           /* Fixed 32-byte header */
#define SRR_CRC_SIZE     2            /* 2-byte CRC-16 */
#define SRR_MIN_FRAME_RAW (SRR_HEADER_SIZE + SRR_CRC_SIZE) /* 34 bytes */

/* Wire Message Types */
typedef enum {
    SRR_WIRE_HELLO    = 1,  /* Session handshake & capability exchange */
    SRR_WIRE_REQUEST  = 2,  /* Synchronous request */
    SRR_WIRE_REPLY    = 3,  /* Synchronous successful reply */
    SRR_WIRE_ERROR    = 4,  /* Explicit error reply */
    SRR_WIRE_PULSE    = 5,  /* Asynchronous one-way pulse */
    SRR_WIRE_CANCEL   = 6,  /* Best-effort transaction cancellation */
    SRR_WIRE_GOODBYE  = 7   /* Orderly disconnect */
} srr_wire_msg_type_t;

/* Architecture-Independent Wire Header */
typedef struct {
    uint32_t magic;         /* Must be SRR_WIRE_MAGIC */
    uint16_t version;       /* Must be SRR_WIRE_VERSION */
    uint16_t msg_type;      /* srr_wire_msg_type_t */
    uint16_t flags;         /* Reserved flags (0) */
    uint16_t service_id;    /* Service namespace ID */
    uint16_t operation_id;  /* Operation ID within service */
    uint16_t reserved;      /* Padding (0) */
    uint32_t session_id;    /* Session ID for reconnect / reboot detection */
    uint64_t trans_id;      /* Transaction correlation identifier */
    uint32_t payload_len;   /* Byte length of application payload */
} srr_wire_header_t;

/*
 * ============================================================================
 * 1. CRC-16-CCITT (False) Algorithm
 * ============================================================================
 * Polynomial: 0x1021, Initial: 0xFFFF, No reflect.
 * Test vector: "123456789" -> 0x29B1
 */
uint16_t srr_crc16(const uint8_t *data, size_t len, uint16_t init_crc);

/*
 * ============================================================================
 * 2. COBS (Consistent Overhead Byte Stuffing)
 * ============================================================================
 */
/*
 * Encodes src into dst using COBS.
 * Maximum output size is src_len + (src_len / 254) + 1.
 * Returns bytes written to dst, or 0 if dst_cap was insufficient.
 */
size_t srr_cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/*
 * Decodes COBS-encoded src into dst.
 * Returns decoded byte count, or 0 if malformed.
 */
size_t srr_cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/*
 * ============================================================================
 * 3. Header Serialization (Little-Endian)
 * ============================================================================
 */
size_t srr_wire_encode_header(const srr_wire_header_t *hdr, uint8_t *dst, size_t dst_cap);
bool   srr_wire_decode_header(const uint8_t *src, size_t src_len, srr_wire_header_t *out_hdr);

/*
 * Encodes a complete frame: [Header (32B)] + [Payload (len)] + [CRC16 (2B)] -> COBS -> [Delimiter (0x00)]
 * Returns total bytes written to out_frame (including trailing 0x00 delimiter).
 */
size_t srr_wire_encode_frame(
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len,
    uint8_t *out_frame,
    size_t out_cap);

/*
 * ============================================================================
 * 4. Streaming Chunk Parser (Zero Heap Allocation)
 * ============================================================================
 */
typedef enum {
    SRR_PARSER_NEED_MORE = 0,    /* Accumulating bytes; frame incomplete */
    SRR_PARSER_FRAME_READY,      /* Valid frame parsed successfully */
    SRR_PARSER_ERR_OVERFLOW,     /* Frame exceeded buffer capacity */
    SRR_PARSER_ERR_CRC,          /* CRC verification failed (corrupted frame) */
    SRR_PARSER_ERR_MALFORMED     /* COBS or header decode error */
} srr_parser_status_t;

typedef struct {
    uint8_t *rx_buf;             /* Buffer for raw COBS bytes */
    size_t   rx_cap;             /* Capacity of rx_buf */
    size_t   rx_len;             /* Current bytes in rx_buf */

    uint8_t *decode_buf;         /* Buffer for decoded un-COBSed bytes */
    size_t   decode_cap;         /* Capacity of decode_buf */

    bool     hunting;            /* Discarding garbage until next delimiter */
} srr_wire_parser_t;

void srr_wire_parser_init(
    srr_wire_parser_t *p,
    uint8_t *rx_buf,
    size_t rx_cap,
    uint8_t *decode_buf,
    size_t decode_cap);

void srr_wire_parser_reset(srr_wire_parser_t *p);

/* Feed one byte into parser */
srr_parser_status_t srr_wire_parser_feed_byte(
    srr_wire_parser_t *p,
    uint8_t byte,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload);

#ifdef __cplusplus
}
#endif

#endif /* SRR_WIRE_H */
