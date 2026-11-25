#ifndef SRR_H
#define SRR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SRR: Synchronous Send / Receive / Reply Communication Library
 * 
 * Core interaction:
 *   1. Client: srr_send() / srr_send_timed() blocks until server replies.
 *   2. Server: srr_receive() returns an incoming request (or pulse).
 *   3. Server: srr_reply() / srr_reply_error() copies reply to client and unblocks client.
 *   4. Server: srr_request_dispose() releases server-side request handle.
 */

/* Project Version */
#define SRR_VERSION_MAJOR    0
#define SRR_VERSION_MINOR    1
#define SRR_VERSION_PATCH    0
#define SRR_VERSION_STRING   "0.1.0"
#define SRR_VERSION_HEX      ((SRR_VERSION_MAJOR << 16) | (SRR_VERSION_MINOR << 8) | (SRR_VERSION_PATCH))

/* Wire & Framing Protocol Version */
#define SRR_PROTOCOL_VERSION 1

/* Runtime Version Queries */
const char* srr_version(void);
uint32_t    srr_version_number(void);
uint16_t    srr_protocol_version(void);

/* Opaque Handles */
typedef struct srr_channel    srr_channel_t;
typedef struct srr_connection srr_connection_t;
typedef struct srr_request    srr_request_t;

/* Status / Error Codes */
typedef enum {
    SRR_OK = 0,
    SRR_TIMEOUT,            /* Send timed out before reply arrived */
    SRR_CANCELLED,          /* Request was cancelled or abandoned */
    SRR_DISCONNECTED,       /* Connection severed */
    SRR_SERVER_DIED,        /* Server closed or crashed before replying */
    SRR_CLOSED,             /* Channel was closed */
    SRR_QUEUE_FULL,         /* Channel queue capacity reached */
    SRR_TOO_LARGE,          /* Message or reply exceeds capacity */
    SRR_PROTOCOL_ERROR,     /* Framing or state machine violation */
    SRR_NO_MEMORY,          /* Memory allocation failure */
    SRR_INVALID             /* Invalid parameter or state */
} srr_status_t;

/* Configuration Defaults */
#define SRR_DEFAULT_QUEUE_CAPACITY 64
#define SRR_DEFAULT_MAX_MSG_SIZE   (64 * 1024)  /* 64 KiB */

/* Channel Options */
typedef struct {
    size_t      queue_capacity;    /* Max pending queued requests (0 = default) */
    size_t      max_request_size;  /* Max bytes per request (0 = default) */
    size_t      max_reply_size;    /* Max bytes per reply (0 = default) */
    const char *service_name;      /* Optional: if set, binds a local IPC service */
} srr_channel_options_t;

/* Connection Options */
typedef struct {
    uint32_t default_timeout_ms; /* 0 = wait indefinitely */
} srr_connect_options_t;

/*
 * ============================================================================
 * Channel API (Server Side)
 * ============================================================================
 */

/*
 * Create a new channel.
 * options may be NULL for defaults. If options->service_name is specified,
 * starts an IPC listener on that service name (Unix domain socket).
 */
srr_status_t srr_channel_create(
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);

/*
 * Create a new IPC service channel explicitly.
 */
srr_status_t srr_channel_create_service(
    const char *service_name,
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);

/*
 * Close channel to new sends and wake all waiting receivers.
 * Blocked receivers return SRR_CLOSED.
 * Queued undelivered requests fail with SRR_SERVER_DIED.
 * Already-delivered requests remain valid until replied or disposed.
 */
srr_status_t srr_channel_close(srr_channel_t *channel);

/*
 * Destroy channel and free internal resources.
 * Safe to call after srr_channel_close(). Outstanding active requests
 * will be cleanly cleaned up via reference counting.
 */
void srr_channel_destroy(srr_channel_t *channel);

/*
 * Receive next request or pulse from channel.
 * Blocks until an item is available or channel closes.
 * On success, *out_request must eventually be completed via srr_reply(),
 * srr_reply_error(), or srr_request_dispose().
 */
srr_status_t srr_receive(
    srr_channel_t *channel,
    srr_request_t **out_request);

/*
 * ============================================================================
 * Connection API (Client Side)
 * ============================================================================
 */

/*
 * Connect to an in-process channel directly.
 */
srr_status_t srr_connect_channel(
    srr_channel_t *channel,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);

/*
 * Connect by service name (e.g. for IPC in Milestone 2, or named channels).
 */
srr_status_t srr_connect(
    const char *service_name,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);

/*
 * Disconnect client connection and wake blocked sends with SRR_DISCONNECTED.
 */
void srr_disconnect(srr_connection_t *connection);

/*
 * Send a synchronous request and block until reply is received.
 * Request data is copied into library storage before send returns or blocks.
 * Reply data is copied into caller-provided reply buffer.
 * If actual reply exceeds reply_capacity, returns SRR_TOO_LARGE and sets
 * *out_reply_size to required capacity.
 */
srr_status_t srr_send(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size);

/*
 * Send with a specific timeout in milliseconds (0 = indefinite).
 * If timeout expires before reply completes, returns SRR_TIMEOUT.
 */
srr_status_t srr_send_timed(
    srr_connection_t *connection,
    const void *request_data,
    size_t request_size,
    void *reply_buffer,
    size_t reply_capacity,
    size_t *out_reply_size,
    uint32_t timeout_ms);

/*
 * Send an asynchronous one-way pulse (no reply expected).
 * Non-blocking: returns SRR_QUEUE_FULL if channel queue is full.
 */
srr_status_t srr_pulse(
    srr_connection_t *connection,
    uint32_t code,
    uint64_t value);

/*
 * ============================================================================
 * Request / Reply API (Server Side Handling)
 * ============================================================================
 */

/* Query if received item is a pulse rather than a request */
bool srr_request_is_pulse(const srr_request_t *request);

/* Pulse data accessors */
uint32_t srr_pulse_code(const srr_request_t *request);
uint64_t srr_pulse_value(const srr_request_t *request);

/* Request data accessors */
const void *srr_request_data(const srr_request_t *request);
size_t      srr_request_size(const srr_request_t *request);

/*
 * Reply to client with data payload.
 * Single-winner: only the first call to reply/reply_error succeeds.
 * If client timed out or disconnected before reply, returns SRR_TIMEOUT
 * or SRR_DISCONNECTED to inform server.
 * Automatically disposes the request handle.
 */
srr_status_t srr_reply(
    srr_request_t *request,
    const void *reply_data,
    size_t reply_size);

/*
 * Reply to client with an explicit error code.
 * Client's srr_send() will return this status code.
 * Automatically disposes the request handle.
 */
srr_status_t srr_reply_error(
    srr_request_t *request,
    srr_status_t error);

/*
 * Explicitly abandon / dispose an unanswered request or pulse.
 * If client is still waiting, client's srr_send() will wake with SRR_CANCELLED.
 * For pulses, frees pulse resources.
 * Safe to call on already replied requests (no-op).
 */
void srr_request_dispose(srr_request_t *request);

/* Convert status code to human-readable string */
const char *srr_status_str(srr_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SRR_H */
