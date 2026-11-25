#include <string.h>
#include "srr/srr_wire.h"

/*
 * ============================================================================
 * 1. CRC-16-CCITT (False)
 * ============================================================================
 */
uint16_t srr_crc16(const uint8_t *data, size_t len, uint16_t init_crc) {
    uint16_t crc = init_crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/*
 * ============================================================================
 * 2. COBS Encode / Decode
 * ============================================================================
 */
size_t srr_cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (!src || !dst) return 0;

    size_t read_idx = 0;
    size_t write_idx = 1;
    size_t code_idx = 0;
    uint8_t code = 1;

    while (read_idx < src_len) {
        if (src[read_idx] == 0) {
            if (code_idx >= dst_cap) return 0;
            dst[code_idx] = code;
            code = 1;
            code_idx = write_idx++;
            read_idx++;
        } else {
            if (write_idx >= dst_cap) return 0;
            dst[write_idx++] = src[read_idx++];
            code++;
            if (code == 0xFF) {
                if (code_idx >= dst_cap) return 0;
                dst[code_idx] = code;
                code = 1;
                code_idx = write_idx++;
            }
        }
    }

    if (code_idx >= dst_cap) return 0;
    dst[code_idx] = code;
    return write_idx;
}

size_t srr_cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (!src || !dst || src_len == 0) return 0;

    size_t read_idx = 0;
    size_t write_idx = 0;

    while (read_idx < src_len) {
        uint8_t code = src[read_idx++];
        if (code == 0) return 0; /* Zero inside COBS data is invalid */

        for (uint8_t i = 1; i < code; i++) {
            if (read_idx >= src_len || write_idx >= dst_cap) return 0;
            dst[write_idx++] = src[read_idx++];
        }

        if (code < 0xFF && read_idx < src_len) {
            if (write_idx >= dst_cap) return 0;
            dst[write_idx++] = 0;
        }
    }

    return write_idx;
}

/*
 * ============================================================================
 * 3. Header Serialization (Little-Endian)
 * ============================================================================
 */
static inline void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void put_u64_le(uint8_t *p, uint64_t v) {
    put_u32_le(p, (uint32_t)(v & 0xFFFFFFFFULL));
    put_u32_le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

static inline uint16_t get_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static inline uint64_t get_u64_le(const uint8_t *p) {
    return (uint64_t)((uint64_t)get_u32_le(p) | ((uint64_t)get_u32_le(p + 4) << 32));
}

size_t srr_wire_encode_header(const srr_wire_header_t *hdr, uint8_t *dst, size_t dst_cap) {
    if (!hdr || !dst || dst_cap < SRR_HEADER_SIZE) return 0;

    put_u32_le(dst + 0,  hdr->magic);
    put_u16_le(dst + 4,  hdr->version);
    put_u16_le(dst + 6,  hdr->msg_type);
    put_u16_le(dst + 8,  hdr->flags);
    put_u16_le(dst + 10, hdr->service_id);
    put_u16_le(dst + 12, hdr->operation_id);
    put_u16_le(dst + 14, hdr->reserved);
    put_u32_le(dst + 16, hdr->session_id);
    put_u64_le(dst + 20, hdr->trans_id);
    put_u32_le(dst + 28, hdr->payload_len);

    return SRR_HEADER_SIZE;
}

bool srr_wire_decode_header(const uint8_t *src, size_t src_len, srr_wire_header_t *out_hdr) {
    if (!src || !out_hdr || src_len < SRR_HEADER_SIZE) return false;

    out_hdr->magic        = get_u32_le(src + 0);
    out_hdr->version      = get_u16_le(src + 4);
    out_hdr->msg_type     = get_u16_le(src + 6);
    out_hdr->flags        = get_u16_le(src + 8);
    out_hdr->service_id   = get_u16_le(src + 10);
    out_hdr->operation_id = get_u16_le(src + 12);
    out_hdr->reserved     = get_u16_le(src + 14);
    out_hdr->session_id   = get_u32_le(src + 16);
    out_hdr->trans_id     = get_u64_le(src + 20);
    out_hdr->payload_len  = get_u32_le(src + 28);

    if (out_hdr->magic != SRR_WIRE_MAGIC || out_hdr->version != SRR_WIRE_VERSION) {
        return false;
    }
    return true;
}

size_t srr_wire_encode_frame(
    const srr_wire_header_t *hdr,
    const void *payload,
    size_t payload_len,
    uint8_t *out_frame,
    size_t out_cap)
{
    if (!hdr || !out_frame) return 0;
    if (payload_len > 0 && !payload) return 0; /* Strictly reject nonzero length with NULL pointer */

    /* Guard against integer overflow and excessive size */
    if (payload_len > 1024 - SRR_HEADER_SIZE - SRR_CRC_SIZE) {
        return 0; /* Exceeds single frame scratch budget */
    }
    size_t unencoded_len = SRR_HEADER_SIZE + payload_len + SRR_CRC_SIZE;

    /* Stack scratch buffer for unencoded bytes */
    uint8_t raw[1024];
    uint8_t *raw_buf = raw;

    srr_wire_header_t local_hdr = *hdr;
    local_hdr.magic = SRR_WIRE_MAGIC;
    local_hdr.version = SRR_WIRE_VERSION;
    local_hdr.payload_len = (uint32_t)payload_len;

    srr_wire_encode_header(&local_hdr, raw_buf, sizeof(raw));
    if (payload_len > 0 && payload) {
        memcpy(raw_buf + SRR_HEADER_SIZE, payload, payload_len);
    }

    /* Compute CRC-16 over Header + Payload */
    uint16_t crc = srr_crc16(raw_buf, SRR_HEADER_SIZE + payload_len, 0xFFFF);
    put_u16_le(raw_buf + SRR_HEADER_SIZE + payload_len, crc);

    /* COBS encode into out_frame */
    size_t cobs_len = srr_cobs_encode(raw_buf, unencoded_len, out_frame, out_cap > 1 ? out_cap - 1 : 0);
    if (cobs_len == 0 || cobs_len + 1 > out_cap) {
        return 0;
    }

    /* Append zero-byte delimiter */
    out_frame[cobs_len] = SRR_WIRE_DELIM;
    return cobs_len + 1;
}

/*
 * ============================================================================
 * 4. Streaming Chunk Parser Implementation
 * ============================================================================
 */
void srr_wire_parser_init(
    srr_wire_parser_t *p,
    uint8_t *rx_buf,
    size_t rx_cap,
    uint8_t *decode_buf,
    size_t decode_cap)
{
    if (!p) return;
    p->rx_buf = rx_buf;
    p->rx_cap = rx_cap;
    p->rx_len = 0;
    p->decode_buf = decode_buf;
    p->decode_cap = decode_cap;
    p->hunting = false;
}

void srr_wire_parser_reset(srr_wire_parser_t *p) {
    if (!p) return;
    p->rx_len = 0;
    p->hunting = false;
}

srr_parser_status_t srr_wire_parser_feed_byte(
    srr_wire_parser_t *p,
    uint8_t byte,
    srr_wire_header_t *out_hdr,
    const uint8_t **out_payload)
{
    if (!p) return SRR_PARSER_ERR_MALFORMED;

    if (byte == SRR_WIRE_DELIM) {
        if (p->hunting) {
            p->hunting = false;
            p->rx_len = 0;
            return SRR_PARSER_NEED_MORE;
        }

        if (p->rx_len == 0) {
            /* Repeated delimiters: ignore */
            return SRR_PARSER_NEED_MORE;
        }

        /* End of COBS frame reached -> decode */
        size_t decoded_len = srr_cobs_decode(p->rx_buf, p->rx_len, p->decode_buf, p->decode_cap);
        p->rx_len = 0; /* Ready for next frame */

        if (decoded_len < SRR_MIN_FRAME_RAW) {
            return SRR_PARSER_ERR_MALFORMED;
        }

        /* Check CRC-16 */
        size_t content_len = decoded_len - SRR_CRC_SIZE;
        uint16_t expected_crc = get_u16_le(p->decode_buf + content_len);
        uint16_t actual_crc = srr_crc16(p->decode_buf, content_len, 0xFFFF);

        if (expected_crc != actual_crc) {
            return SRR_PARSER_ERR_CRC;
        }

        /* Decode Header */
        if (!srr_wire_decode_header(p->decode_buf, content_len, out_hdr)) {
            return SRR_PARSER_ERR_MALFORMED;
        }

        if (out_hdr->payload_len != content_len - SRR_HEADER_SIZE) {
            return SRR_PARSER_ERR_MALFORMED;
        }

        if (out_payload) {
            *out_payload = (out_hdr->payload_len > 0) ? (p->decode_buf + SRR_HEADER_SIZE) : NULL;
        }

        return SRR_PARSER_FRAME_READY;
    }

    /* Non-delimiter byte */
    if (p->hunting) {
        return SRR_PARSER_NEED_MORE;
    }

    if (p->rx_len >= p->rx_cap) {
        /* Buffer overflow: start hunting for next delimiter */
        p->hunting = true;
        p->rx_len = 0;
        return SRR_PARSER_ERR_OVERFLOW;
    }

    p->rx_buf[p->rx_len++] = byte;
    return SRR_PARSER_NEED_MORE;
}
