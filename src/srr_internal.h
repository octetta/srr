#ifndef SRR_INTERNAL_H
#define SRR_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/srr/srr.h"

typedef enum {
    SRR_BACKEND_THREAD = 0,
    SRR_BACKEND_IPC
} srr_backend_type_t;

typedef enum {
    SRR_ITEM_REQUEST,
    SRR_ITEM_PULSE
} srr_item_type_t;

typedef enum {
    SRR_STATE_CREATED = 0,
    SRR_STATE_QUEUED,
    SRR_STATE_DELIVERED,
    /* Terminal states below */
    SRR_STATE_REPLIED,
    SRR_STATE_CANCELLED,
    SRR_STATE_TIMED_OUT,
    SRR_STATE_DISCONNECTED,
    SRR_STATE_SERVER_DIED,
    SRR_STATE_ERROR
} srr_trans_state_t;

struct srr_request {
    srr_backend_type_t backend;
    srr_item_type_t    type;
    srr_trans_state_t  state;
    srr_status_t       terminal_status;

    /* Synchronization for thread sender */
    pthread_mutex_t    lock;
    pthread_cond_t     cond;
    int                ref_count;  /* 1 for client, 1 for queue/server */

    /* Pulse payload */
    uint32_t           pulse_code;
    uint64_t           pulse_value;

    /* Request payload (owned by request) */
    void              *req_data;
    size_t             req_size;

    /* Reply storage (for in-process thread sender) */
    void              *reply_buf;
    size_t             reply_cap;
    size_t            *out_reply_size;
    size_t             actual_reply_size;

    /* IPC backend peer info (fd on POSIX, HANDLE on Windows) */
    intptr_t           ipc_client_handle;
    uint64_t           ipc_trans_id;
    pthread_mutex_t   *ipc_client_lock; /* serializes writes to client handle */
    void              *ipc_session;     /* ref-counted ipc_client_session_t */

    /* Queue linkage */
    struct srr_request *next;
};

typedef struct ipc_client_session ipc_client_session_t;
#ifndef _WIN32
void srr_ipc_session_release(ipc_client_session_t *sess);
#endif

typedef enum {
    SRR_CHAN_OPEN = 0,
    SRR_CHAN_CLOSING,
    SRR_CHAN_CLOSED
} srr_chan_state_t;

struct ipc_client_node;

struct srr_channel {
    pthread_mutex_t    lock;
    pthread_cond_t     recv_cond;    /* Wakes waiting receivers */
    srr_chan_state_t   state;
    int                ref_count;

    /* Queue */
    struct srr_request *q_head;
    struct srr_request *q_tail;
    size_t             q_count;
    size_t             q_capacity;

    /* Config limits */
    size_t             max_req_size;
    size_t             max_reply_size;

    /* Receiver metrics */
    size_t             waiting_receivers;

    /* IPC State (Unix Domain Sockets on POSIX, Named Pipes on Windows) */
    bool               has_ipc;
    char              *ipc_path;
    intptr_t           ipc_listen_handle;
    pthread_t          ipc_listen_th;
    pthread_mutex_t    ipc_clients_lock;
    struct ipc_client_node *ipc_clients;
};

struct srr_connection {
    pthread_mutex_t    send_lock;    /* Serializes concurrent sends on this connection */
    bool               is_ipc;

    /* In-process backend */
    srr_channel_t     *channel;      /* Referenced channel */

    /* IPC backend */
    intptr_t           ipc_handle;
    uint64_t           next_trans_id;

    bool               connected;
    uint32_t           default_timeout_ms;
};

/* Helper forward declarations */
srr_request_t *srr_request_alloc(srr_backend_type_t backend, srr_item_type_t type);
void           srr_request_retain(srr_request_t *req);
void           srr_request_release(srr_request_t *req);
bool           srr_request_complete_terminal(srr_request_t *req, srr_trans_state_t term_state, srr_status_t status);

void           srr_channel_retain(srr_channel_t *chan);
void           srr_channel_release(srr_channel_t *chan);

/* IPC helpers */
int            srr_ipc_resolve_path(const char *service_name, char *out_path, size_t out_path_cap);
srr_status_t   srr_ipc_server_start(srr_channel_t *channel, const char *service_name);
void           srr_ipc_server_stop(srr_channel_t *channel);

int            srr_ipc_write_all(intptr_t handle, const void *buf, size_t len, int timeout_ms);
int            srr_ipc_read_all(intptr_t handle, void *buf, size_t len, int timeout_ms);

#endif /* SRR_INTERNAL_H */
