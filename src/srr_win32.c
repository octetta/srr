#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "srr_internal.h"
#include "srr_proto.h"

typedef struct ipc_client_node {
    pthread_t               thread;
    HANDLE                  pipe_handle;
    struct ipc_client_node *next;
} ipc_client_node_t;

int srr_ipc_resolve_path(const char *service_name, char *out_path, size_t out_path_cap) {
    if (!service_name || !out_path || out_path_cap == 0) return -1;
    if (strncmp(service_name, "\\\\.\\pipe\\", 9) == 0) {
        snprintf(out_path, out_path_cap, "%s", service_name);
    } else {
        snprintf(out_path, out_path_cap, "\\\\.\\pipe\\srr_%s", service_name);
    }
    return 0;
}

int srr_ipc_write_all(intptr_t handle, const void *buf, size_t len, int timeout_ms) {
    (void)timeout_ms;
    HANDLE h = (HANDLE)handle;
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        DWORD written = 0;
        DWORD to_write = (DWORD)(remaining > 65536 ? 65536 : remaining);
        if (!WriteFile(h, p, to_write, &written, NULL) || written == 0) {
            return -1;
        }
        p += written;
        remaining -= written;
    }
    return 0;
}

int srr_ipc_read_all(intptr_t handle, void *buf, size_t len, int timeout_ms) {
    (void)timeout_ms;
    HANDLE h = (HANDLE)handle;
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        DWORD read_bytes = 0;
        DWORD to_read = (DWORD)(remaining > 65536 ? 65536 : remaining);
        if (!ReadFile(h, p, to_read, &read_bytes, NULL) || read_bytes == 0) {
            return -1;
        }
        p += read_bytes;
        remaining -= read_bytes;
    }
    return 0;
}

typedef struct {
    srr_channel_t   *channel;
    HANDLE           pipe_handle;
    pthread_mutex_t  client_write_lock;
} win32_pipe_session_t;

static void *win32_pipe_session_thread(void *arg) {
    win32_pipe_session_t *sess = (win32_pipe_session_t *)arg;
    srr_channel_t *chan = sess->channel;
    HANDLE h = sess->pipe_handle;

    while (1) {
        srr_frame_header_t hdr;
        if (srr_ipc_read_all((intptr_t)h, &hdr, sizeof(hdr), -1) != 0) {
            break;
        }

        if (hdr.magic != SRR_PROTO_MAGIC || hdr.version != SRR_PROTO_VERSION) {
            break;
        }

        if (hdr.msg_type == SRR_MSG_GOODBYE) {
            break;
        }

        if (hdr.msg_type == SRR_MSG_PULSE) {
            srr_pulse_payload_t pl;
            if (hdr.payload_len != sizeof(pl)) break;
            if (srr_ipc_read_all((intptr_t)h, &pl, sizeof(pl), 5000) != 0) break;

            srr_request_t *pulse = srr_request_alloc(SRR_BACKEND_THREAD, SRR_ITEM_PULSE);
            if (pulse) {
                pulse->pulse_code = pl.code;
                pulse->pulse_value = pl.value;
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
                srr_frame_header_t err_hdr = {
                    .magic = SRR_PROTO_MAGIC,
                    .version = SRR_PROTO_VERSION,
                    .msg_type = SRR_MSG_ERROR,
                    .flags = 0,
                    .trans_id = hdr.trans_id,
                    .payload_len = sizeof(srr_error_payload_t)
                };
                srr_error_payload_t err_pl = { .error_status = (uint32_t)SRR_TOO_LARGE, ._reserved = 0 };
                srr_ipc_write_all((intptr_t)h, &err_hdr, sizeof(err_hdr), 5000);
                srr_ipc_write_all((intptr_t)h, &err_pl, sizeof(err_pl), 5000);
                break;
            }

            void *req_data = NULL;
            if (hdr.payload_len > 0) {
                req_data = malloc(hdr.payload_len);
                if (!req_data) break;
                if (srr_ipc_read_all((intptr_t)h, req_data, hdr.payload_len, 5000) != 0) {
                    free(req_data);
                    break;
                }
            }

            srr_request_t *req = srr_request_alloc(SRR_BACKEND_IPC, SRR_ITEM_REQUEST);
            if (!req) {
                free(req_data);
                break;
            }

            req->req_data = req_data;
            req->req_size = hdr.payload_len;
            req->ipc_client_handle = (intptr_t)h;
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
                srr_ipc_write_all((intptr_t)h, &err_hdr, sizeof(err_hdr), 5000);
                srr_ipc_write_all((intptr_t)h, &err_pl, sizeof(err_pl), 5000);
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

    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
    pthread_mutex_destroy(&sess->client_write_lock);
    srr_channel_release(sess->channel);
    free(sess);
    return NULL;
}

static void *win32_pipe_listener_thread(void *arg) {
    srr_channel_t *chan = (srr_channel_t *)arg;

    while (1) {
        HANDLE hPipe = CreateNamedPipeA(
            chan->ipc_path,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        pthread_mutex_lock(&chan->lock);
        if (chan->state != SRR_CHAN_OPEN || !connected) {
            pthread_mutex_unlock(&chan->lock);
            CloseHandle(hPipe);
            break;
        }
        chan->ref_count++;
        pthread_mutex_unlock(&chan->lock);

        win32_pipe_session_t *sess = (win32_pipe_session_t *)calloc(1, sizeof(win32_pipe_session_t));
        if (!sess) {
            CloseHandle(hPipe);
            srr_channel_release(chan);
            continue;
        }
        sess->channel = chan;
        sess->pipe_handle = hPipe;
        pthread_mutex_init(&sess->client_write_lock, NULL);

        ipc_client_node_t *node = (ipc_client_node_t *)malloc(sizeof(ipc_client_node_t));
        if (!node) {
            CloseHandle(hPipe);
            pthread_mutex_destroy(&sess->client_write_lock);
            free(sess);
            srr_channel_release(chan);
            continue;
        }
        node->pipe_handle = hPipe;

        if (pthread_create(&node->thread, NULL, win32_pipe_session_thread, sess) != 0) {
            CloseHandle(hPipe);
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
    char path[256];
    if (srr_ipc_resolve_path(service_name, path, sizeof(path)) != 0) {
        return SRR_INVALID;
    }

    channel->has_ipc = true;
    channel->ipc_path = strdup(path);
    channel->ipc_clients = NULL;
    pthread_mutex_init(&channel->ipc_clients_lock, NULL);

    channel->ref_count++;
    if (pthread_create(&channel->ipc_listen_th, NULL, win32_pipe_listener_thread, channel) != 0) {
        channel->ref_count--;
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

    /* Create dummy connection to unblock listener ConnectNamedPipe */
    HANDLE hDummy = CreateFileA(channel->ipc_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDummy != INVALID_HANDLE_VALUE) {
        CloseHandle(hDummy);
    }

    pthread_join(channel->ipc_listen_th, NULL);

    /* Cancel and close all active client pipes */
    pthread_mutex_lock(&channel->ipc_clients_lock);
    ipc_client_node_t *curr = channel->ipc_clients;
    while (curr) {
        CancelIoEx(curr->pipe_handle, NULL);
        curr = curr->next;
    }
    pthread_mutex_unlock(&channel->ipc_clients_lock);

    curr = channel->ipc_clients;
    while (curr) {
        ipc_client_node_t *next = curr->next;
        pthread_join(curr->thread, NULL);
        free(curr);
        curr = next;
    }
    channel->ipc_clients = NULL;
    pthread_mutex_destroy(&channel->ipc_clients_lock);

    if (channel->ipc_path) {
        free(channel->ipc_path);
        channel->ipc_path = NULL;
    }

    channel->has_ipc = false;
    srr_channel_release(channel);
}

#endif /* _WIN32 */
