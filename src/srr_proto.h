#ifndef SRR_PROTO_H
#define SRR_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define SRR_PROTO_MAGIC   0x53525231U /* "SRR1" */
#define SRR_PROTO_VERSION 1

typedef enum {
    SRR_MSG_HELLO = 1,
    SRR_MSG_REQUEST,
    SRR_MSG_REPLY,
    SRR_MSG_ERROR,
    SRR_MSG_PULSE,
    SRR_MSG_CANCEL,
    SRR_MSG_GOODBYE
} srr_msg_type_t;

/* Packed wire frame header (24 bytes) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* SRR_PROTO_MAGIC */
    uint16_t version;       /* SRR_PROTO_VERSION */
    uint16_t msg_type;      /* srr_msg_type_t */
    uint32_t flags;         /* Reserved (0) */
    uint64_t trans_id;      /* Transaction correlation ID */
    uint32_t payload_len;   /* Length of payload following header */
} srr_frame_header_t;

typedef struct {
    uint32_t code;
    uint32_t _reserved;
    uint64_t value;
} srr_pulse_payload_t;

typedef struct {
    uint32_t error_status;  /* srr_status_t */
    uint32_t _reserved;
} srr_error_payload_t;
#pragma pack(pop)

#endif /* SRR_PROTO_H */
