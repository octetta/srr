#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "srr_internal.h"
#include "srr_proto.h"

typedef struct ipc_client_node {
    pthread_t               thread;
    int                     client_fd;
    struct ipc_client_node *next;
} ipc_client_node_t;

int srr_ipc_resolve_path(const char *service_name, char *out_path, size_t out_path_cap) {
    if (!service_name || !out_path || out_path_cap == 0) return -1;
    if (strchr(service_name, '/')) {
        snprintf(out_path, out_path_cap, "%s", service_name);
    } else {
        snprintf(out_path, out_path_cap, "/tmp/srr_%s.sock", service_name);
    }
    return 0;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int srr_ipc_write_all(intptr_t handle, const void *buf, size_t len, int timeout_ms) {
    int fd = (int)handle;
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        if (timeout_ms >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int ret = poll(&pfd, 1, timeout_ms);
            if (ret <= 0) {
                return (ret == 0) ? -2 : -1; /* -2 = timeout, -1 = error */
            }
        }
        ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

int srr_ipc_read_all(intptr_t handle, void *buf, size_t len, int timeout_ms) {
    int fd = (int)handle;
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, (timeout_ms >= 0) ? timeout_ms : -1);
        if (ret <= 0) {
            return (ret == 0) ? -2 : -1; /* -2 = timeout, -1 = error */
        }
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* EOF */
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

struct ipc_client_session {
    srr_channel_t   *channel;
    int              client_fd;
    pthread_mutex_t  client_write_lock;
    pthread_mutex_t  ref_lock;
    int              ref_count;
};

void srr_ipc_session_release(ipc_client_session_t *sess) {
    if (!sess) return;
    pthread_mutex_lock(&sess->ref_lock);
    sess->ref_count--;
    int count = sess->ref_count;
    pthread_mutex_unlock(&sess->ref_lock);
    if (count <= 0) {
        if (sess->client_fd >= 0) {
            close(sess->client_fd);
            sess->client_fd = -1;
        }
        pthread_mutex_destroy(&sess->client_write_lock);
        pthread_mutex_destroy(&sess->ref_lock);
        free(sess);
    }
}

static void *ipc_client_session_thread(void *arg) {
    ipc_client_session_t *sess = (ipc_client_session_t *)arg;
    srr_channel_t *chan = sess->channel;
    int fd = sess->client_fd;

    while (1) {
        srr_frame_header_t hdr;
        int rc = srr_ipc_read_all(fd, &hdr, sizeof(hdr), -1);
        if (rc != 0) {
            break; /* Client disconnected, closed, or EOF */
        }

        if (hdr.magic != SRR_PROTO_MAGIC || hdr.version != SRR_PROTO_VERSION) {
            break; /* Malformed protocol */
        }

        if (hdr.msg_type == SRR_MSG_GOODBYE) {
            break;
        }

        if (hdr.msg_type == SRR_MSG_PULSE) {
            srr_pulse_payload_t pulse_payload;
            if (hdr.payload_len != sizeof(pulse_payload)) break;
            if (srr_ipc_read_all(fd, &pulse_payload, sizeof(pulse_payload), 5000) != 0) break;

            srr_request_t *pulse = srr_request_alloc(SRR_BACKEND_THREAD, SRR_ITEM_PULSE);
            if (pulse) {
                pulse->pulse_code = pulse_payload.code;
                pulse->pulse_value = pulse_payload.value;
                pulse->state = SRR_STATE_QUEUED;

                pthread_mutex_lock(&chan->lock);
                if (chan->state == SRR_CHAN_OPEN && chan->q_count < chan->q_capacity) {
                    pulse->next = NULL;
                    if (chan->q_tail) chan->q_tail->next = pulse;
                    else chan->q_head = pulse;
                    chan->q_tail = pulse;
                    chan->q_count++;
                    pthread_cond_signal(&chan->recv_cond);
                    pthread_mutex_unlock(&chan->lock);
                } else {
                    pthread_mutex_unlock(&chan->lock);
                    srr_request_release(pulse);
                }
            }
            continue;
        }

        if (hdr.msg_type == SRR_MSG_REQUEST) {
            if (hdr.payload_len > chan->max_req_size) {
                /* Oversized request: reply with error */
                srr_frame_header_t err_hdr = {
                    .magic = SRR_PROTO_MAGIC,
                    .version = SRR_PROTO_VERSION,
                    .msg_type = SRR_MSG_ERROR,
                    .flags = 0,
                    .trans_id = hdr.trans_id,
                    .payload_len = sizeof(srr_error_payload_t)
                };
                srr_error_payload_t err_pl = { .error_status = (uint32_t)SRR_TOO_LARGE, ._reserved = 0 };
                srr_ipc_write_all(fd, &err_hdr, sizeof(err_hdr), 5000);
                srr_ipc_write_all(fd, &err_pl, sizeof(err_pl), 5000);
                break;
            }

            void *req_data = NULL;
            if (hdr.payload_len > 0) {
                req_data = malloc(hdr.payload_len);
                if (!req_data) break;
                if (srr_ipc_read_all(fd, req_data, hdr.payload_len, 5000) != 0) {
                    free(req_data);
                    break;
                }
            }

            srr_request_t *req = srr_request_alloc(SRR_BACKEND_IPC, SRR_ITEM_REQUEST);
            if (!req) {
                free(req_data);
                break;
            }

            pthread_mutex_lock(&sess->ref_lock);
            sess->ref_count++;
            pthread_mutex_unlock(&sess->ref_lock);

            req->ipc_session = sess;
            req->req_data = req_data;
            req->req_size = hdr.payload_len;
            req->ipc_client_handle = (intptr_t)fd;
            req->ipc_trans_id = hdr.trans_id;
            req->ipc_client_lock = &sess->client_write_lock;
            req->state = SRR_STATE_QUEUED;

            pthread_mutex_lock(&chan->lock);
            if (chan->state != SRR_CHAN_OPEN) {
                pthread_mutex_unlock(&chan->lock);
                srr_request_release(req);
                break;
            }

            if (chan->q_count >= chan->q_capacity) {
                pthread_mutex_unlock(&chan->lock);
                /* Queue full */
                srr_frame_header_t err_hdr = {
                    .magic = SRR_PROTO_MAGIC,
                    .version = SRR_PROTO_VERSION,
                    .msg_type = SRR_MSG_ERROR,
                    .flags = 0,
                    .trans_id = hdr.trans_id,
                    .payload_len = sizeof(srr_error_payload_t)
                };
                srr_error_payload_t err_pl = { .error_status = (uint32_t)SRR_QUEUE_FULL, ._reserved = 0 };
                pthread_mutex_lock(&sess->client_write_lock);
                srr_ipc_write_all(fd, &err_hdr, sizeof(err_hdr), 5000);
                srr_ipc_write_all(fd, &err_pl, sizeof(err_pl), 5000);
                pthread_mutex_unlock(&sess->client_write_lock);
                srr_request_release(req);
                continue;
            }

            req->next = NULL;
            if (chan->q_tail) chan->q_tail->next = req;
            else chan->q_head = req;
            chan->q_tail = req;
            chan->q_count++;

            pthread_cond_signal(&chan->recv_cond);
            pthread_mutex_unlock(&chan->lock);
        }
    }

    srr_channel_release(sess->channel);
    srr_ipc_session_release(sess);
    return NULL;
}

static void *ipc_listener_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;

    while (1) {
        int client_fd = accept((int)chan->ipc_listen_handle, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break; /* Channel closed or shutdown */
        }

        pthread_mutex_lock(&chan->lock);
        if (chan->state != SRR_CHAN_OPEN) {
            pthread_mutex_unlock(&chan->lock);
            close(client_fd);
            break;
        }
        chan->ref_count++;
        pthread_mutex_unlock(&chan->lock);

        ipc_client_session_t *sess = (ipc_client_session_t *)calloc(1, sizeof(ipc_client_session_t));
        if (!sess) {
            close(client_fd);
            srr_channel_release(chan);
            continue;
        }
        sess->channel = chan;
        sess->client_fd = client_fd;
        sess->ref_count = 1;
        pthread_mutex_init(&sess->ref_lock, NULL);
        pthread_mutex_init(&sess->client_write_lock, NULL);

        ipc_client_node_t *node = (ipc_client_node_t *)malloc(sizeof(ipc_client_node_t));
        if (!node) {
            close(client_fd);
            sess->client_fd = -1;
            pthread_mutex_destroy(&sess->client_write_lock);
            pthread_mutex_destroy(&sess->ref_lock);
            free(sess);
            srr_channel_release(chan);
            continue;
        }
        node->client_fd = client_fd;

        if (pthread_create(&node->thread, NULL, ipc_client_session_thread, sess) != 0) {
            close(client_fd);
            pthread_mutex_destroy(&sess->client_write_lock);
            free(sess);
            free(node);
            srr_channel_release(chan);
            continue;
        }

        pthread_mutex_lock(&chan->ipc_clients_lock);
        node->next = chan->ipc_clients;
        chan->ipc_clients = node;
        pthread_mutex_unlock(&chan->ipc_clients_lock);
    }
    return NULL;
}

srr_status_t srr_ipc_server_start(srr_channel_t *channel, const char *service_name) {
    char path[104];
    if (srr_ipc_resolve_path(service_name, path, sizeof(path)) != 0) {
        return SRR_INVALID;
    }

    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return SRR_NO_MEMORY;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        return SRR_INVALID;
    }

    if (listen(listen_fd, 32) != 0) {
        close(listen_fd);
        unlink(path);
        return SRR_INVALID;
    }

    channel->has_ipc = true;
    channel->ipc_path = strdup(path);
    channel->ipc_listen_handle = (intptr_t)listen_fd;
    channel->ipc_clients = NULL;
    pthread_mutex_init(&channel->ipc_clients_lock, NULL);

    /* Retain reference for listener thread */
    channel->ref_count++;
    if (pthread_create(&channel->ipc_listen_th, NULL, ipc_listener_thread, channel) != 0) {
        channel->ref_count--;
        close(listen_fd);
        unlink(path);
        free(channel->ipc_path);
        channel->ipc_path = NULL;
        channel->has_ipc = false;
        pthread_mutex_destroy(&channel->ipc_clients_lock);
        return SRR_NO_MEMORY;
    }

    return SRR_OK;
}

void srr_ipc_server_stop(srr_channel_t *channel) {
    if (!channel || !channel->has_ipc) return;

    int fd = (int)channel->ipc_listen_handle;
    channel->ipc_listen_handle = -1;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    if (channel->ipc_path) {
        unlink(channel->ipc_path);
        free(channel->ipc_path);
        channel->ipc_path = NULL;
    }

    pthread_join(channel->ipc_listen_th, NULL);

    /* Shutdown all client connection fds to interrupt reads */
    pthread_mutex_lock(&channel->ipc_clients_lock);
    ipc_client_node_t *curr = channel->ipc_clients;
    while (curr) {
        shutdown(curr->client_fd, SHUT_RDWR);
        curr = curr->next;
    }
    pthread_mutex_unlock(&channel->ipc_clients_lock);

    /* Join all client session threads */
    curr = channel->ipc_clients;
    while (curr) {
        ipc_client_node_t *next = curr->next;
        pthread_join(curr->thread, NULL);
        free(curr);
        curr = next;
    }
    channel->ipc_clients = NULL;
    pthread_mutex_destroy(&channel->ipc_clients_lock);

    channel->has_ipc = false;
    /* Release reference held by listener thread */
    srr_channel_release(channel);
}

#endif /* !_WIN32 */
