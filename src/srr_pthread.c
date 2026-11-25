#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#include "srr_internal.h"
#include "srr_proto.h"

/*
 * ============================================================================
 * Internal Request / Transaction Lifecycle
 * ============================================================================
 */

srr_request_t *srr_request_alloc(srr_backend_type_t backend, srr_item_type_t type) {
    srr_request_t *req = (srr_request_t *)calloc(1, sizeof(srr_request_t));
    if (!req) return NULL;

    if (pthread_mutex_init(&req->lock, NULL) != 0) {
        free(req);
        return NULL;
    }
    if (pthread_cond_init(&req->cond, NULL) != 0) {
        pthread_mutex_destroy(&req->lock);
        free(req);
        return NULL;
    }

    req->backend = backend;
    req->type = type;
    req->state = SRR_STATE_CREATED;
    req->terminal_status = SRR_OK;
    req->ref_count = 1;
    return req;
}

void srr_request_retain(srr_request_t *req) {
    if (!req) return;
    pthread_mutex_lock(&req->lock);
    req->ref_count++;
    pthread_mutex_unlock(&req->lock);
}

void srr_request_release(srr_request_t *req) {
    if (!req) return;
    pthread_mutex_lock(&req->lock);
    int rc = --req->ref_count;
    pthread_mutex_unlock(&req->lock);

    if (rc <= 0) {
        pthread_cond_destroy(&req->cond);
        pthread_mutex_destroy(&req->lock);
        if (req->req_data) {
            free(req->req_data);
            req->req_data = NULL;
        }
#ifndef _WIN32
        if (req->backend == SRR_BACKEND_IPC && req->ipc_session) {
            srr_ipc_session_release((ipc_client_session_t *)req->ipc_session);
            req->ipc_session = NULL;
        }
#endif
        free(req);
    }
}

bool srr_request_complete_terminal(srr_request_t *req, srr_trans_state_t term_state, srr_status_t status) {
    if (!req) return false;
    pthread_mutex_lock(&req->lock);
    if (req->state >= SRR_STATE_REPLIED) {
        pthread_mutex_unlock(&req->lock);
        return false;
    }

    req->state = term_state;
    req->terminal_status = status;
    pthread_cond_broadcast(&req->cond);
    pthread_mutex_unlock(&req->lock);
    return true;
}

/*
 * ============================================================================
 * Channel Implementation
 * ============================================================================
 */

void srr_channel_retain(srr_channel_t *chan) {
    if (!chan) return;
    pthread_mutex_lock(&chan->lock);
    chan->ref_count++;
    pthread_mutex_unlock(&chan->lock);
}

void srr_channel_release(srr_channel_t *chan) {
    if (!chan) return;
    pthread_mutex_lock(&chan->lock);
    int rc = --chan->ref_count;
    pthread_mutex_unlock(&chan->lock);

    if (rc <= 0) {
        pthread_cond_destroy(&chan->recv_cond);
        pthread_mutex_destroy(&chan->lock);
        free(chan);
    }
}

srr_status_t srr_channel_create(
    const srr_channel_options_t *options,
    srr_channel_t **out_channel)
{
    if (!out_channel) return SRR_INVALID;

    srr_channel_t *chan = (srr_channel_t *)calloc(1, sizeof(srr_channel_t));
    if (!chan) return SRR_NO_MEMORY;

    if (pthread_mutex_init(&chan->lock, NULL) != 0) {
        free(chan);
        return SRR_NO_MEMORY;
    }
    if (pthread_cond_init(&chan->recv_cond, NULL) != 0) {
        pthread_mutex_destroy(&chan->lock);
        free(chan);
        return SRR_NO_MEMORY;
    }

    chan->state = SRR_CHAN_OPEN;
    chan->ref_count = 1;
    chan->q_capacity = (options && options->queue_capacity > 0)
        ? options->queue_capacity : SRR_DEFAULT_QUEUE_CAPACITY;
    chan->max_req_size = (options && options->max_request_size > 0)
        ? options->max_request_size : SRR_DEFAULT_MAX_MSG_SIZE;
    chan->max_reply_size = (options && options->max_reply_size > 0)
        ? options->max_reply_size : SRR_DEFAULT_MAX_MSG_SIZE;

    if (options && options->service_name) {
        srr_status_t rc = srr_ipc_server_start(chan, options->service_name);
        if (rc != SRR_OK) {
            pthread_cond_destroy(&chan->recv_cond);
            pthread_mutex_destroy(&chan->lock);
            free(chan);
            return rc;
        }
    }

    *out_channel = chan;
    return SRR_OK;
}

srr_status_t srr_channel_create_service(
    const char *service_name,
    const srr_channel_options_t *options,
    srr_channel_t **out_channel)
{
    srr_channel_options_t opts;
    if (options) {
        opts = *options;
    } else {
        memset(&opts, 0, sizeof(opts));
    }
    opts.service_name = service_name;
    return srr_channel_create(&opts, out_channel);
}

srr_status_t srr_channel_close(srr_channel_t *channel) {
    if (!channel) return SRR_INVALID;

    if (channel->has_ipc) {
        srr_ipc_server_stop(channel);
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->state == SRR_CHAN_CLOSED) {
        pthread_mutex_unlock(&channel->lock);
        return SRR_OK;
    }

    channel->state = SRR_CHAN_CLOSED;
    pthread_cond_broadcast(&channel->recv_cond);

    srr_request_t *curr = channel->q_head;
    channel->q_head = NULL;
    channel->q_tail = NULL;
    channel->q_count = 0;
    pthread_mutex_unlock(&channel->lock);

    while (curr) {
        srr_request_t *next = curr->next;
        curr->next = NULL;
        if (curr->type == SRR_ITEM_REQUEST) {
            if (curr->backend == SRR_BACKEND_THREAD) {
                srr_request_complete_terminal(curr, SRR_STATE_SERVER_DIED, SRR_SERVER_DIED);
            }
        }
        srr_request_release(curr);
        curr = next;
    }

    return SRR_OK;
}

void srr_channel_destroy(srr_channel_t *channel) {
    if (!channel) return;
    srr_channel_close(channel);
    srr_channel_release(channel);
}

srr_status_t srr_receive(
    srr_channel_t *channel,
    srr_request_t **out_request)
{
    if (!channel || !out_request) return SRR_INVALID;
    *out_request = NULL;

    pthread_mutex_lock(&channel->lock);

    while (true) {
        while (channel->q_head != NULL) {
            srr_request_t *req = channel->q_head;
            channel->q_head = req->next;
            if (!channel->q_head) {
                channel->q_tail = NULL;
            }
            req->next = NULL;
            channel->q_count--;

            if (req->type == SRR_ITEM_REQUEST && req->backend == SRR_BACKEND_THREAD) {
                pthread_mutex_lock(&req->lock);
                if (req->state >= SRR_STATE_REPLIED) {
                    pthread_mutex_unlock(&req->lock);
                    srr_request_release(req);
                    continue;
                }
                req->state = SRR_STATE_DELIVERED;
                pthread_mutex_unlock(&req->lock);
            } else if (req->type == SRR_ITEM_REQUEST) {
                req->state = SRR_STATE_DELIVERED;
            }

            pthread_mutex_unlock(&channel->lock);
            *out_request = req;
            return SRR_OK;
        }

        if (channel->state != SRR_CHAN_OPEN) {
            pthread_mutex_unlock(&channel->lock);
            return SRR_CLOSED;
        }

        channel->waiting_receivers++;
        pthread_cond_wait(&channel->recv_cond, &channel->lock);
        channel->waiting_receivers--;
    }
}

/*
 * ============================================================================
 * Connection Implementation
 * ============================================================================
 */

srr_status_t srr_connect_channel(
    srr_channel_t *channel,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection)
{
    if (!channel || !out_connection) return SRR_INVALID;

    pthread_mutex_lock(&channel->lock);
    if (channel->state != SRR_CHAN_OPEN) {
        pthread_mutex_unlock(&channel->lock);
        return SRR_CLOSED;
    }
    channel->ref_count++;
    pthread_mutex_unlock(&channel->lock);

    srr_connection_t *conn = (srr_connection_t *)calloc(1, sizeof(srr_connection_t));
    if (!conn) {
        srr_channel_release(channel);
        return SRR_NO_MEMORY;
    }

    if (pthread_mutex_init(&conn->send_lock, NULL) != 0) {
        srr_channel_release(channel);
        free(conn);
        return SRR_NO_MEMORY;
    }

    conn->is_ipc = false;
    conn->channel = channel;
    conn->connected = true;
    conn->default_timeout_ms = options ? options->default_timeout_ms : 0;

    *out_connection = conn;
    return SRR_OK;
}

srr_status_t srr_connect(
    const char *service_name,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection)
{
    if (!service_name || !out_connection) return SRR_INVALID;

#ifdef _WIN32
    char path[256];
    if (srr_ipc_resolve_path(service_name, path, sizeof(path)) != 0) {
        return SRR_INVALID;
    }

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return SRR_DISCONNECTED;
    }

    srr_connection_t *conn = (srr_connection_t *)calloc(1, sizeof(srr_connection_t));
    if (!conn) {
        CloseHandle(h);
        return SRR_NO_MEMORY;
    }

    if (pthread_mutex_init(&conn->send_lock, NULL) != 0) {
        CloseHandle(h);
        free(conn);
        return SRR_NO_MEMORY;
    }

    conn->is_ipc = true;
    conn->ipc_handle = (intptr_t)h;
    conn->connected = true;
    conn->default_timeout_ms = options ? options->default_timeout_ms : 0;

    *out_connection = conn;
    return SRR_OK;

#else /* POSIX */

    char path[104];
    if (srr_ipc_resolve_path(service_name, path, sizeof(path)) != 0) {
        return SRR_INVALID;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return SRR_NO_MEMORY;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return SRR_DISCONNECTED;
    }

    srr_connection_t *conn = (srr_connection_t *)calloc(1, sizeof(srr_connection_t));
    if (!conn) {
        close(fd);
        return SRR_NO_MEMORY;
    }

    if (pthread_mutex_init(&conn->send_lock, NULL) != 0) {
        close(fd);
        free(conn);
        return SRR_NO_MEMORY;
    }

    conn->is_ipc = true;
    conn->ipc_handle = (intptr_t)fd;
    conn->connected = true;
    conn->default_timeout_ms = options ? options->default_timeout_ms : 0;

    *out_connection = conn;
    return SRR_OK;
#endif
}

void srr_disconnect(srr_connection_t *connection) {
    if (!connection) return;

    pthread_mutex_lock(&connection->send_lock);
    if (!connection->connected) {
        pthread_mutex_unlock(&connection->send_lock);
        return;
    }

    connection->connected = false;

    if (connection->is_ipc) {
        srr_frame_header_t bye = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_GOODBYE,
            .flags = 0,
            .trans_id = 0,
            .payload_len = 0
        };
        srr_ipc_write_all(connection->ipc_handle, &bye, sizeof(bye), 1000);
#ifdef _WIN32
        CloseHandle((HANDLE)connection->ipc_handle);
#else
        close((int)connection->ipc_handle);
#endif
        connection->ipc_handle = 0;
        pthread_mutex_unlock(&connection->send_lock);
        pthread_mutex_destroy(&connection->send_lock);
        free(connection);
        return;
    }

    srr_channel_t *chan = connection->channel;
    connection->channel = NULL;
    pthread_mutex_unlock(&connection->send_lock);

    if (chan) {
        srr_channel_release(chan);
    }
    pthread_mutex_destroy(&connection->send_lock);
    free(connection);
}

static void compute_timespec(uint32_t timeout_ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    uint64_t nsec = (uint64_t)ts->tv_nsec + ((uint64_t)timeout_ms * 1000000ULL);
    ts->tv_sec += (time_t)(nsec / 1000000000ULL);
    ts->tv_nsec = (long)(nsec % 1000000000ULL);
}

srr_status_t srr_send_timed(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size,
    uint32_t timeout_ms)
{
    if (!connection) return SRR_INVALID;
    if (request_size > 0 && !request_data) return SRR_INVALID;
    if (reply_capacity > 0 && !reply_buffer) return SRR_INVALID;

    pthread_mutex_lock(&connection->send_lock);
    if (!connection->connected) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_DISCONNECTED;
    }

    if (connection->is_ipc) {
        intptr_t h = connection->ipc_handle;
        uint64_t tid = ++connection->next_trans_id;
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_REQUEST,
            .flags = 0,
            .trans_id = tid,
            .payload_len = (uint32_t)request_size
        };

        if (srr_ipc_write_all(h, &hdr, sizeof(hdr), (timeout_ms > 0) ? (int)timeout_ms : -1) != 0) {
            pthread_mutex_unlock(&connection->send_lock);
            return SRR_DISCONNECTED;
        }

        if (request_size > 0) {
            if (srr_ipc_write_all(h, request_data, request_size, (timeout_ms > 0) ? (int)timeout_ms : -1) != 0) {
                pthread_mutex_unlock(&connection->send_lock);
                return SRR_DISCONNECTED;
            }
        }

        srr_frame_header_t rep_hdr;
        int rrc = srr_ipc_read_all(h, &rep_hdr, sizeof(rep_hdr), (timeout_ms > 0) ? (int)timeout_ms : -1);
        if (rrc == -2) {
            pthread_mutex_unlock(&connection->send_lock);
            return SRR_TIMEOUT;
        }
        if (rrc != 0 || rep_hdr.magic != SRR_PROTO_MAGIC) {
            pthread_mutex_unlock(&connection->send_lock);
            return SRR_DISCONNECTED;
        }

        if (rep_hdr.msg_type == SRR_MSG_ERROR) {
            srr_error_payload_t err_pl;
            if (srr_ipc_read_all(h, &err_pl, sizeof(err_pl), 5000) != 0) {
                pthread_mutex_unlock(&connection->send_lock);
                return SRR_DISCONNECTED;
            }
            pthread_mutex_unlock(&connection->send_lock);
            return (srr_status_t)err_pl.error_status;
        }

        if (rep_hdr.msg_type == SRR_MSG_REPLY) {
            if (out_reply_size) *out_reply_size = rep_hdr.payload_len;

            if (rep_hdr.payload_len > reply_capacity) {
                size_t to_copy = reply_capacity;
                if (to_copy > 0) {
                    if (srr_ipc_read_all(h, reply_buffer, to_copy, 5000) != 0) {
                        pthread_mutex_unlock(&connection->send_lock);
                        return SRR_DISCONNECTED;
                    }
                }
                size_t remaining = rep_hdr.payload_len - to_copy;
                char discard[256];
                while (remaining > 0) {
                    size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
                    if (srr_ipc_read_all(h, discard, chunk, 5000) != 0) {
                        pthread_mutex_unlock(&connection->send_lock);
                        return SRR_DISCONNECTED;
                    }
                    remaining -= chunk;
                }
                pthread_mutex_unlock(&connection->send_lock);
                return SRR_TOO_LARGE;
            } else {
                if (rep_hdr.payload_len > 0) {
                    if (srr_ipc_read_all(h, reply_buffer, rep_hdr.payload_len, 5000) != 0) {
                        pthread_mutex_unlock(&connection->send_lock);
                        return SRR_DISCONNECTED;
                    }
                }
                pthread_mutex_unlock(&connection->send_lock);
                return SRR_OK;
            }
        }

        pthread_mutex_unlock(&connection->send_lock);
        return SRR_PROTOCOL_ERROR;
    }

    /* In-Process Thread backend */
    srr_channel_t *chan = connection->channel;
    if (!chan) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_DISCONNECTED;
    }

    if (request_size > chan->max_req_size) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_TOO_LARGE;
    }

    srr_request_t *req = srr_request_alloc(SRR_BACKEND_THREAD, SRR_ITEM_REQUEST);
    if (!req) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_NO_MEMORY;
    }

    if (request_size > 0) {
        req->req_data = malloc(request_size);
        if (!req->req_data) {
            srr_request_release(req);
            pthread_mutex_unlock(&connection->send_lock);
            return SRR_NO_MEMORY;
        }
        memcpy(req->req_data, request_data, request_size);
        req->req_size = request_size;
    }

    req->reply_buf = reply_buffer;
    req->reply_cap = reply_capacity;
    req->out_reply_size = out_reply_size;

    srr_request_retain(req);

    pthread_mutex_lock(&chan->lock);
    if (chan->state != SRR_CHAN_OPEN) {
        pthread_mutex_unlock(&chan->lock);
        srr_request_release(req);
        srr_request_release(req);
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_CLOSED;
    }

    if (chan->q_count >= chan->q_capacity) {
        pthread_mutex_unlock(&chan->lock);
        srr_request_release(req);
        srr_request_release(req);
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_QUEUE_FULL;
    }

    req->state = SRR_STATE_QUEUED;
    req->next = NULL;
    if (chan->q_tail) {
        chan->q_tail->next = req;
    } else {
        chan->q_head = req;
    }
    chan->q_tail = req;
    chan->q_count++;

    pthread_cond_signal(&chan->recv_cond);
    pthread_mutex_unlock(&chan->lock);

    struct timespec ts;
    if (timeout_ms > 0) {
        compute_timespec(timeout_ms, &ts);
    }

    pthread_mutex_lock(&req->lock);
    while (req->state < SRR_STATE_REPLIED) {
        if (timeout_ms == 0) {
            pthread_cond_wait(&req->cond, &req->lock);
        } else {
            int rc = pthread_cond_timedwait(&req->cond, &req->lock, &ts);
            if (rc == ETIMEDOUT && req->state < SRR_STATE_REPLIED) {
                req->state = SRR_STATE_TIMED_OUT;
                req->terminal_status = SRR_TIMEOUT;
                pthread_cond_broadcast(&req->cond);
                break;
            }
        }
    }

    srr_status_t outcome = req->terminal_status;
    pthread_mutex_unlock(&req->lock);

    srr_request_release(req);
    pthread_mutex_unlock(&connection->send_lock);
    return outcome;
}

srr_status_t srr_send(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size)
{
    uint32_t to = connection ? connection->default_timeout_ms : 0;
    return srr_send_timed(
        connection,
        request_data,
        request_size,
        reply_buffer,
        reply_capacity,
        out_reply_size,
        to);
}

srr_status_t srr_pulse(
    srr_connection_t *connection,
    uint32_t code,
    uint64_t value)
{
    if (!connection) return SRR_INVALID;

    pthread_mutex_lock(&connection->send_lock);
    if (!connection->connected) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_DISCONNECTED;
    }

    if (connection->is_ipc) {
        srr_frame_header_t hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_PULSE,
            .flags = 0,
            .trans_id = 0,
            .payload_len = sizeof(srr_pulse_payload_t)
        };
        srr_pulse_payload_t pl = { .code = code, ._reserved = 0, .value = value };
        srr_ipc_write_all(connection->ipc_handle, &hdr, sizeof(hdr), 5000);
        srr_ipc_write_all(connection->ipc_handle, &pl, sizeof(pl), 5000);
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_OK;
    }

    srr_channel_t *chan = connection->channel;
    srr_request_t *pulse = srr_request_alloc(SRR_BACKEND_THREAD, SRR_ITEM_PULSE);
    if (!pulse) {
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_NO_MEMORY;
    }

    pulse->pulse_code = code;
    pulse->pulse_value = value;
    pulse->state = SRR_STATE_QUEUED;

    pthread_mutex_lock(&chan->lock);
    if (chan->state != SRR_CHAN_OPEN) {
        pthread_mutex_unlock(&chan->lock);
        srr_request_release(pulse);
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_CLOSED;
    }

    if (chan->q_count >= chan->q_capacity) {
        pthread_mutex_unlock(&chan->lock);
        srr_request_release(pulse);
        pthread_mutex_unlock(&connection->send_lock);
        return SRR_QUEUE_FULL;
    }

    pulse->next = NULL;
    if (chan->q_tail) {
        chan->q_tail->next = pulse;
    } else {
        chan->q_head = pulse;
    }
    chan->q_tail = pulse;
    chan->q_count++;

    pthread_cond_signal(&chan->recv_cond);
    pthread_mutex_unlock(&chan->lock);
    pthread_mutex_unlock(&connection->send_lock);

    return SRR_OK;
}

/*
 * ============================================================================
 * Server Request Handling & Reply
 * ============================================================================
 */

bool srr_request_is_pulse(const srr_request_t *request) {
    return request && request->type == SRR_ITEM_PULSE;
}

uint32_t srr_pulse_code(const srr_request_t *request) {
    return request ? request->pulse_code : 0;
}

uint64_t srr_pulse_value(const srr_request_t *request) {
    return request ? request->pulse_value : 0;
}

const void *srr_request_data(const srr_request_t *request) {
    return request ? request->req_data : NULL;
}

size_t srr_request_size(const srr_request_t *request) {
    return request ? request->req_size : 0;
}

srr_status_t srr_reply(
    srr_request_t *request,
    const void *reply_data,
    size_t reply_size)
{
    if (!request) return SRR_INVALID;
    if (request->type == SRR_ITEM_PULSE) {
        srr_request_dispose(request);
        return SRR_INVALID;
    }

    if (request->backend == SRR_BACKEND_IPC) {
        pthread_mutex_lock(request->ipc_client_lock);
        srr_frame_header_t rep_hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_REPLY,
            .flags = 0,
            .trans_id = request->ipc_trans_id,
            .payload_len = (uint32_t)reply_size
        };
        srr_ipc_write_all(request->ipc_client_handle, &rep_hdr, sizeof(rep_hdr), 5000);
        if (reply_size > 0 && reply_data) {
            srr_ipc_write_all(request->ipc_client_handle, reply_data, reply_size, 5000);
        }
        pthread_mutex_unlock(request->ipc_client_lock);
        srr_request_release(request);
        return SRR_OK;
    }

    pthread_mutex_lock(&request->lock);
    if (request->state >= SRR_STATE_REPLIED) {
        srr_status_t past_status = request->terminal_status;
        pthread_mutex_unlock(&request->lock);
        srr_request_release(request);
        return past_status;
    }

    if (request->out_reply_size) {
        *request->out_reply_size = reply_size;
    }

    srr_status_t reply_result = SRR_OK;

    if (reply_size > request->reply_cap) {
        request->terminal_status = SRR_TOO_LARGE;
        request->state = SRR_STATE_ERROR;
        reply_result = SRR_TOO_LARGE;
    } else {
        if (reply_size > 0 && request->reply_buf && reply_data) {
            memcpy(request->reply_buf, reply_data, reply_size);
        }
        request->terminal_status = SRR_OK;
        request->state = SRR_STATE_REPLIED;
    }

    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);

    srr_request_release(request);
    return reply_result;
}

srr_status_t srr_reply_error(
    srr_request_t *request,
    srr_status_t error)
{
    if (!request) return SRR_INVALID;
    if (request->type == SRR_ITEM_PULSE) {
        srr_request_dispose(request);
        return SRR_INVALID;
    }

    if (request->backend == SRR_BACKEND_IPC) {
        pthread_mutex_lock(request->ipc_client_lock);
        srr_frame_header_t err_hdr = {
            .magic = SRR_PROTO_MAGIC,
            .version = SRR_PROTO_VERSION,
            .msg_type = SRR_MSG_ERROR,
            .flags = 0,
            .trans_id = request->ipc_trans_id,
            .payload_len = sizeof(srr_error_payload_t)
        };
        srr_error_payload_t err_pl = { .error_status = (uint32_t)error, ._reserved = 0 };
        srr_ipc_write_all(request->ipc_client_handle, &err_hdr, sizeof(err_hdr), 5000);
        srr_ipc_write_all(request->ipc_client_handle, &err_pl, sizeof(err_pl), 5000);
        pthread_mutex_unlock(request->ipc_client_lock);
        srr_request_release(request);
        return SRR_OK;
    }

    pthread_mutex_lock(&request->lock);
    if (request->state >= SRR_STATE_REPLIED) {
        srr_status_t past_status = request->terminal_status;
        pthread_mutex_unlock(&request->lock);
        srr_request_release(request);
        return past_status;
    }

    request->terminal_status = error;
    request->state = SRR_STATE_ERROR;
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);

    srr_request_release(request);
    return SRR_OK;
}

void srr_request_dispose(srr_request_t *request) {
    if (!request) return;

    if (request->type == SRR_ITEM_REQUEST && request->backend == SRR_BACKEND_THREAD) {
        pthread_mutex_lock(&request->lock);
        if (request->state < SRR_STATE_REPLIED) {
            request->state = SRR_STATE_CANCELLED;
            request->terminal_status = SRR_CANCELLED;
            pthread_cond_broadcast(&request->cond);
        }
        pthread_mutex_unlock(&request->lock);
    }

    srr_request_release(request);
}

const char *srr_status_str(srr_status_t status) {
    switch (status) {
        case SRR_OK:             return "SRR_OK";
        case SRR_TIMEOUT:        return "SRR_TIMEOUT";
        case SRR_CANCELLED:      return "SRR_CANCELLED";
        case SRR_DISCONNECTED:   return "SRR_DISCONNECTED";
        case SRR_SERVER_DIED:    return "SRR_SERVER_DIED";
        case SRR_CLOSED:         return "SRR_CLOSED";
        case SRR_QUEUE_FULL:     return "SRR_QUEUE_FULL";
        case SRR_TOO_LARGE:      return "SRR_TOO_LARGE";
        case SRR_PROTOCOL_ERROR: return "SRR_PROTOCOL_ERROR";
        case SRR_NO_MEMORY:      return "SRR_NO_MEMORY";
        case SRR_INVALID:        return "SRR_INVALID";
        default:                 return "SRR_UNKNOWN";
    }
}
