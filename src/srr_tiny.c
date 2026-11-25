#include "srr_tiny.h"
#include <string.h>

void srr_tiny_init(
    srr_tiny_endpoint_t *ep,
    uint32_t session_id,
    srr_tiny_tx_fn_t tx_fn,
    void *tx_user_data)
{
    if (!ep) return;
    memset(ep, 0, sizeof(srr_tiny_endpoint_t));
    ep->session_id = session_id;
    ep->tx_fn = tx_fn;
    ep->tx_user_data = tx_user_data;

    srr_wire_parser_init(
        &ep->parser,
        ep->rx_buf, sizeof(ep->rx_buf),
        ep->decode_buf, sizeof(ep->decode_buf)
    );
}

int srr_tiny_register_service(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    srr_tiny_handler_t handler,
    void *user_data)
{
    if (!ep || !handler || ep->service_count >= SRR_TINY_MAX_SERVICES) return -1;

    for (uint8_t i = 0; i < ep->service_count; i++) {
        if (ep->services[i].service_id == service_id) {
            ep->services[i].handler = handler;
            ep->services[i].user_data = user_data;
            return 0;
        }
    }

    ep->services[ep->service_count].service_id = service_id;
    ep->services[ep->service_count].handler = handler;
    ep->services[ep->service_count].user_data = user_data;
    ep->service_count++;
    return 0;
}

srr_parser_status_t srr_tiny_feed_byte(srr_tiny_endpoint_t *ep, uint8_t byte) {
    if (!ep) return SRR_PARSER_ERR_MALFORMED;

    srr_wire_header_t in_hdr;
    const uint8_t *in_payload = NULL;

    srr_parser_status_t st = srr_wire_parser_feed_byte(&ep->parser, byte, &in_hdr, &in_payload);
    if (st != SRR_PARSER_FRAME_READY) {
        return st;
    }

    if (in_hdr.magic != SRR_WIRE_MAGIC || in_hdr.version != SRR_WIRE_VERSION) {
        return SRR_PARSER_ERR_MALFORMED;
    }

    if (in_hdr.msg_type == SRR_WIRE_REQUEST) {
        srr_tiny_handler_t handler = NULL;
        void *user_data = NULL;

        for (uint8_t i = 0; i < ep->service_count; i++) {
            if (ep->services[i].service_id == in_hdr.service_id) {
                handler = ep->services[i].handler;
                user_data = ep->services[i].user_data;
                break;
            }
        }

        srr_wire_header_t rep_hdr;
        memset(&rep_hdr, 0, sizeof(rep_hdr));
        rep_hdr.magic = SRR_WIRE_MAGIC;
        rep_hdr.version = SRR_WIRE_VERSION;
        rep_hdr.session_id = ep->session_id;
        rep_hdr.trans_id = in_hdr.trans_id;
        rep_hdr.service_id = in_hdr.service_id;
        rep_hdr.operation_id = in_hdr.operation_id;

        uint8_t rep_len = 0;
        int handler_rc = -1;

        if (handler) {
            uint8_t safe_req_len = (uint8_t)(in_hdr.payload_len < SRR_TINY_MAX_PAYLOAD ? in_hdr.payload_len : SRR_TINY_MAX_PAYLOAD);
            handler_rc = handler(
                in_hdr.service_id,
                in_hdr.operation_id,
                in_payload,
                safe_req_len,
                ep->server_rep_buf,
                sizeof(ep->server_rep_buf),
                &rep_len,
                user_data
            );
        }

        if (handler && handler_rc == 0) {
            rep_hdr.msg_type = SRR_WIRE_REPLY;
            rep_hdr.payload_len = rep_len;
            size_t enc = srr_wire_encode_frame(&rep_hdr, ep->server_rep_buf, rep_len, ep->tx_buf, sizeof(ep->tx_buf));
            if (enc > 0 && ep->tx_fn) {
                ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
            }
        } else {
            rep_hdr.msg_type = SRR_WIRE_ERROR;
            rep_hdr.payload_len = 0;
            size_t enc = srr_wire_encode_frame(&rep_hdr, NULL, 0, ep->tx_buf, sizeof(ep->tx_buf));
            if (enc > 0 && ep->tx_fn) {
                ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
            }
        }
    } else if (in_hdr.msg_type == SRR_WIRE_REPLY || in_hdr.msg_type == SRR_WIRE_ERROR) {
        if (ep->client_state == SRR_TINY_STATE_PENDING && ep->client_trans_id == in_hdr.trans_id) {
            if (in_hdr.msg_type == SRR_WIRE_REPLY) {
                uint8_t cpy = (uint8_t)(in_hdr.payload_len < sizeof(ep->client_reply_buf) ? in_hdr.payload_len : sizeof(ep->client_reply_buf));
                if (in_payload && cpy > 0) {
                    memcpy(ep->client_reply_buf, in_payload, cpy);
                }
                ep->client_reply_len = cpy;
                ep->client_error = 0;
                ep->client_state = SRR_TINY_STATE_REPLIED;
            } else {
                ep->client_error = -1;
                ep->client_state = SRR_TINY_STATE_ERROR;
            }
        }
    }

    return SRR_PARSER_FRAME_READY;
}

int srr_tiny_send_pulse(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    uint8_t payload_len)
{
    if (!ep || !ep->tx_fn) return -1;
    if (payload_len > SRR_TINY_MAX_PAYLOAD) return -1;

    srr_wire_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SRR_WIRE_MAGIC;
    hdr.version = SRR_WIRE_VERSION;
    hdr.msg_type = SRR_WIRE_PULSE;
    hdr.session_id = ep->session_id;
    hdr.trans_id = 0;
    hdr.service_id = service_id;
    hdr.operation_id = operation_id;
    hdr.payload_len = payload_len;

    size_t enc = srr_wire_encode_frame(&hdr, payload, payload_len, ep->tx_buf, sizeof(ep->tx_buf));
    if (enc == 0) return -1;

    return ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
}

uint64_t srr_tiny_begin_request(
    srr_tiny_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    uint8_t req_len)
{
    if (!ep || !ep->tx_fn) return 0;
    if (ep->client_state == SRR_TINY_STATE_PENDING) return 0; /* Busy */
    if (req_len > SRR_TINY_MAX_PAYLOAD) return 0;

    static uint64_t s_next_tid = 1;
    uint64_t tid = s_next_tid++;

    srr_wire_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SRR_WIRE_MAGIC;
    hdr.version = SRR_WIRE_VERSION;
    hdr.msg_type = SRR_WIRE_REQUEST;
    hdr.session_id = ep->session_id;
    hdr.trans_id = tid;
    hdr.service_id = service_id;
    hdr.operation_id = operation_id;
    hdr.payload_len = req_len;

    size_t enc = srr_wire_encode_frame(&hdr, req_payload, req_len, ep->tx_buf, sizeof(ep->tx_buf));
    if (enc == 0) return 0;

    int rc = ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
    if (rc != 0) return 0;

    ep->client_trans_id = tid;
    ep->client_state = SRR_TINY_STATE_PENDING;
    ep->client_reply_len = 0;
    ep->client_error = 0;

    return tid;
}

bool srr_tiny_poll_reply(
    srr_tiny_endpoint_t *ep,
    uint64_t trans_id,
    uint8_t *out_reply,
    uint8_t reply_cap,
    uint8_t *out_reply_len,
    int *out_error)
{
    if (!ep || ep->client_trans_id != trans_id) return false;

    if (ep->client_state == SRR_TINY_STATE_REPLIED) {
        uint8_t cpy = ep->client_reply_len < reply_cap ? ep->client_reply_len : reply_cap;
        if (out_reply && cpy > 0) {
            memcpy(out_reply, ep->client_reply_buf, cpy);
        }
        if (out_reply_len) *out_reply_len = ep->client_reply_len;
        if (out_error) *out_error = 0;
        ep->client_state = SRR_TINY_STATE_IDLE;
        return true;
    } else if (ep->client_state == SRR_TINY_STATE_ERROR) {
        if (out_error) *out_error = ep->client_error;
        ep->client_state = SRR_TINY_STATE_IDLE;
        return true;
    }

    return false; /* Still pending */
}
