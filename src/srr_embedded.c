#include "srr_embedded.h"
#include <string.h>

int srr_endpoint_init(
    srr_endpoint_t *ep,
    uint32_t session_id,
    srr_tx_fn_t tx_fn,
    void *tx_user_data,
    uint8_t *rx_buf,
    size_t rx_cap,
    uint8_t *decode_buf,
    size_t decode_cap,
    uint8_t *tx_buf,
    size_t tx_cap,
    uint8_t *server_reply_buf,
    size_t server_reply_cap)
{
    if (!ep || !tx_fn || !rx_buf || !decode_buf || !tx_buf || !server_reply_buf) {
        return -1;
    }

    memset(ep, 0, sizeof(srr_endpoint_t));
    ep->session_id = session_id;
    ep->next_trans_id = 1;
    ep->tx_fn = tx_fn;
    ep->tx_user_data = tx_user_data;

    ep->tx_buf = tx_buf;
    ep->tx_cap = tx_cap;
    ep->server_reply_buf = server_reply_buf;
    ep->server_reply_cap = server_reply_cap;

    srr_wire_parser_init(&ep->parser, rx_buf, rx_cap, decode_buf, decode_cap);
    return 0;
}

int srr_endpoint_register_service(
    srr_endpoint_t *ep,
    uint16_t service_id,
    srr_service_handler_t handler,
    void *user_data)
{
    if (!ep || !handler) return -1;
    if (ep->service_count >= SRR_EMBEDDED_MAX_SERVICES) return -1;

    for (size_t i = 0; i < ep->service_count; i++) {
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

int srr_endpoint_register_pulse_handler(
    srr_endpoint_t *ep,
    srr_pulse_handler_t handler,
    void *user_data)
{
    if (!ep) return -1;
    ep->pulse_handler = handler;
    ep->pulse_user_data = user_data;
    return 0;
}

srr_parser_status_t srr_endpoint_feed_byte(srr_endpoint_t *ep, uint8_t byte) {
    if (!ep) return SRR_PARSER_ERR_MALFORMED;

    srr_wire_header_t in_hdr;
    const uint8_t *in_payload = NULL;

    srr_parser_status_t st = srr_wire_parser_feed_byte(&ep->parser, byte, &in_hdr, &in_payload);
    if (st != SRR_PARSER_FRAME_READY) {
        if (st == SRR_PARSER_ERR_CRC || st == SRR_PARSER_ERR_MALFORMED || st == SRR_PARSER_ERR_OVERFLOW) {
            ep->rx_errors++;
        }
        return st;
    }

    ep->rx_frames++;

    if (in_hdr.magic != SRR_WIRE_MAGIC || in_hdr.version != SRR_WIRE_VERSION) {
        ep->rx_errors++;
        return SRR_PARSER_ERR_MALFORMED;
    }

    if (in_hdr.msg_type == SRR_WIRE_REQUEST) {
        /* Route request to registered service */
        srr_service_handler_t handler = NULL;
        void *user_data = NULL;

        for (size_t i = 0; i < ep->service_count; i++) {
            if (ep->services[i].service_id == in_hdr.service_id) {
                handler = ep->services[i].handler;
                user_data = ep->services[i].user_data;
                break;
            }
        }

        srr_wire_header_t reply_hdr;
        memset(&reply_hdr, 0, sizeof(reply_hdr));
        reply_hdr.magic = SRR_WIRE_MAGIC;
        reply_hdr.version = SRR_WIRE_VERSION;
        reply_hdr.session_id = ep->session_id;
        reply_hdr.trans_id = in_hdr.trans_id;
        reply_hdr.service_id = in_hdr.service_id;
        reply_hdr.operation_id = in_hdr.operation_id;

        size_t reply_len = 0;
        int handler_rc = -1;

        if (handler) {
            handler_rc = handler(
                in_hdr.service_id,
                in_hdr.operation_id,
                in_payload,
                in_hdr.payload_len,
                ep->server_reply_buf,
                ep->server_reply_cap,
                &reply_len,
                user_data
            );
        }

        if (handler && handler_rc == 0) {
            if (reply_len > ep->server_reply_cap) {
                /* Reply length exceeds allocated server reply buffer: reject with error */
                reply_hdr.msg_type = SRR_WIRE_ERROR;
                reply_hdr.flags = 7; /* SRR_TOO_LARGE */
                reply_hdr.payload_len = 0;
                size_t enc = srr_wire_encode_frame(&reply_hdr, NULL, 0, ep->tx_buf, ep->tx_cap);
                if (enc > 0 && ep->tx_fn) {
                    ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
                    ep->tx_frames++;
                }
            } else {
                reply_hdr.msg_type = SRR_WIRE_REPLY;
                reply_hdr.payload_len = (uint32_t)reply_len;
                size_t enc = srr_wire_encode_frame(&reply_hdr, ep->server_reply_buf, reply_len, ep->tx_buf, ep->tx_cap);
                if (enc > 0 && ep->tx_fn) {
                    ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
                    ep->tx_frames++;
                }
            }
        } else {
            /* Service error */
            reply_hdr.msg_type = SRR_WIRE_ERROR;
            reply_hdr.payload_len = 0;
            size_t enc = srr_wire_encode_frame(&reply_hdr, NULL, 0, ep->tx_buf, ep->tx_cap);
            if (enc > 0 && ep->tx_fn) {
                ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
                ep->tx_frames++;
            }
        }
    } else if (in_hdr.msg_type == SRR_WIRE_REPLY || in_hdr.msg_type == SRR_WIRE_ERROR) {
        /* Match against pending client transaction (suppress duplicates if already transitioned) */
        for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
            srr_async_trans_t *t = &ep->transactions[i];
            if (t->state == SRR_TRANS_PENDING && t->trans_id == in_hdr.trans_id) {
                /* Validate session ID match if provided */
                if (t->session_id != 0 && in_hdr.session_id != 0 && in_hdr.session_id != t->session_id) {
                    t->state = SRR_TRANS_ERROR;
                    t->error_code = -1; /* Session mismatch */
                    break;
                }

                if (in_hdr.msg_type == SRR_WIRE_REPLY) {
                    size_t cpy = in_hdr.payload_len < t->reply_cap ? in_hdr.payload_len : t->reply_cap;
                    if (t->reply_buf && in_payload && cpy > 0) {
                        memcpy(t->reply_buf, in_payload, cpy);
                    }
                    t->reply_len = in_hdr.payload_len;

                    /* Validate reply buffer capacity */
                    if (in_hdr.payload_len > t->reply_cap) {
                        t->error_code = 7; /* SRR_TOO_LARGE */
                        t->state = SRR_TRANS_ERROR;
                    } else {
                        t->error_code = 0;
                        t->state = SRR_TRANS_REPLIED;
                    }
                } else {
                    t->state = SRR_TRANS_ERROR;
                    t->error_code = in_hdr.flags > 0 ? (int)in_hdr.flags : -1;
                }
                break;
            }
        }
    } else if (in_hdr.msg_type == SRR_WIRE_PULSE) {
        if (ep->pulse_handler) {
            ep->pulse_handler(in_hdr.service_id, in_hdr.operation_id, in_payload, in_hdr.payload_len, ep->pulse_user_data);
        }
    }

    return SRR_PARSER_FRAME_READY;
}

void srr_endpoint_feed_bytes(srr_endpoint_t *ep, const uint8_t *data, size_t len) {
    if (!ep || !data) return;
    for (size_t i = 0; i < len; i++) {
        srr_endpoint_feed_byte(ep, data[i]);
    }
}

void srr_endpoint_tick(srr_endpoint_t *ep, uint32_t now_ms) {
    if (!ep) return;
    for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
        srr_async_trans_t *t = &ep->transactions[i];
        if (t->state == SRR_TRANS_PENDING) {
            /* Signed difference handles 32-bit modular timer wraparound correctly */
            int32_t diff = (int32_t)(now_ms - t->timeout_at_ms);
            if (diff >= 0) {
                t->state = SRR_TRANS_TIMED_OUT;
            }
        }
    }
}

int srr_endpoint_send_pulse(
    srr_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len)
{
    if (!ep || !ep->tx_fn) return -1;

    srr_wire_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SRR_WIRE_MAGIC;
    hdr.version = SRR_WIRE_VERSION;
    hdr.msg_type = SRR_WIRE_PULSE;
    hdr.session_id = ep->session_id;
    hdr.trans_id = 0;
    hdr.service_id = service_id;
    hdr.operation_id = operation_id;
    hdr.payload_len = (uint32_t)payload_len;

    size_t enc = srr_wire_encode_frame(&hdr, payload, payload_len, ep->tx_buf, ep->tx_cap);
    if (enc == 0) return -1;

    int rc = ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
    if (rc == 0) ep->tx_frames++;
    return rc;
}

uint64_t srr_begin_request(
    srr_endpoint_t *ep,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    uint8_t *reply_buf,
    size_t reply_cap,
    uint32_t timeout_ms,
    uint32_t now_ms)
{
    if (!ep || !ep->tx_fn) return 0;

    srr_async_trans_t *slot = NULL;
    for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
        if (ep->transactions[i].state == SRR_TRANS_UNUSED) {
            slot = &ep->transactions[i];
            break;
        }
    }
    if (!slot) return 0; /* All transaction slots in flight */

    uint64_t tid = ep->next_trans_id++;

    /* Register pending transaction slot before transmitting request */
    slot->trans_id = tid;
    slot->session_id = ep->session_id;
    slot->state = SRR_TRANS_PENDING;
    slot->service_id = service_id;
    slot->operation_id = operation_id;
    slot->reply_buf = reply_buf;
    slot->reply_cap = reply_cap;
    slot->reply_len = 0;
    slot->error_code = 0;
    slot->timeout_at_ms = now_ms + timeout_ms;

    srr_wire_header_t req_hdr;
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.magic = SRR_WIRE_MAGIC;
    req_hdr.version = SRR_WIRE_VERSION;
    req_hdr.msg_type = SRR_WIRE_REQUEST;
    req_hdr.session_id = ep->session_id;
    req_hdr.trans_id = tid;
    req_hdr.service_id = service_id;
    req_hdr.operation_id = operation_id;
    req_hdr.payload_len = (uint32_t)req_len;

    size_t enc = srr_wire_encode_frame(&req_hdr, req_payload, req_len, ep->tx_buf, ep->tx_cap);
    if (enc == 0) {
        /* Rollback pending slot on encode failure */
        memset(slot, 0, sizeof(*slot));
        return 0;
    }

    int tx_rc = ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
    if (tx_rc != 0) {
        /* Rollback pending slot on transmit failure */
        memset(slot, 0, sizeof(*slot));
        return 0;
    }

    ep->tx_frames++;
    return tid;
}

srr_trans_state_t srr_poll_transaction(srr_endpoint_t *ep, uint64_t trans_id) {
    if (!ep || trans_id == 0) return SRR_TRANS_UNUSED;
    for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
        if (ep->transactions[i].trans_id == trans_id) {
            return ep->transactions[i].state;
        }
    }
    return SRR_TRANS_UNUSED;
}

int srr_collect_reply(
    srr_endpoint_t *ep,
    uint64_t trans_id,
    size_t *out_reply_len,
    int *out_error_code)
{
    if (!ep || trans_id == 0) return -1;
    for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
        srr_async_trans_t *t = &ep->transactions[i];
        if (t->trans_id == trans_id) {
            if (t->state == SRR_TRANS_REPLIED) {
                if (out_reply_len) *out_reply_len = t->reply_len;
                if (out_error_code) *out_error_code = 0;
                memset(t, 0, sizeof(*t));
                return 0;
            } else if (t->state == SRR_TRANS_ERROR) {
                if (out_error_code) *out_error_code = t->error_code;
                memset(t, 0, sizeof(*t));
                return -1;
            } else if (t->state == SRR_TRANS_TIMED_OUT) {
                memset(t, 0, sizeof(*t));
                return -2;
            } else if (t->state == SRR_TRANS_CANCELLED) {
                if (out_error_code) *out_error_code = 2; /* SRR_CANCELLED */
                memset(t, 0, sizeof(*t));
                return -3;
            }
            return -1;
        }
    }
    return -1;
}

int srr_cancel_transaction(srr_endpoint_t *ep, uint64_t trans_id) {
    if (!ep || trans_id == 0) return -1;
    for (size_t i = 0; i < SRR_EMBEDDED_MAX_TRANSACTIONS; i++) {
        srr_async_trans_t *t = &ep->transactions[i];
        if (t->trans_id == trans_id) {
            if (ep->tx_fn && ep->tx_buf && ep->tx_cap >= SRR_MIN_FRAME_RAW) {
                srr_wire_header_t cancel_hdr;
                memset(&cancel_hdr, 0, sizeof(cancel_hdr));
                cancel_hdr.magic = SRR_WIRE_MAGIC;
                cancel_hdr.version = SRR_WIRE_VERSION;
                cancel_hdr.msg_type = SRR_WIRE_CANCEL;
                cancel_hdr.session_id = ep->session_id;
                cancel_hdr.trans_id = trans_id;
                cancel_hdr.service_id = t->service_id;
                cancel_hdr.operation_id = t->operation_id;
                size_t enc = srr_wire_encode_frame(&cancel_hdr, NULL, 0, ep->tx_buf, ep->tx_cap);
                if (enc > 0) {
                    ep->tx_fn(ep->tx_buf, enc, ep->tx_user_data);
                }
            }
            memset(t, 0, sizeof(*t));
            return 0;
        }
    }
    return -1;
}
