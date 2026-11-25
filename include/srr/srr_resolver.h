#ifndef SRR_RESOLVER_H
#define SRR_RESOLVER_H

#include "srr.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SRR_RESOLVER_DEFAULT_SERVICE "srr_resolver"
#define SRR_RESOLVER_DEFAULT_UDP_PORT 8889
#define SRR_RESOLVER_MAX_ENTRIES      128

typedef enum {
    SRR_TRANSPORT_INPROC = 0,
    SRR_TRANSPORT_IPC    = 1,
    SRR_TRANSPORT_SERIAL = 2,
    SRR_TRANSPORT_UDP    = 3
} srr_transport_type_t;

typedef struct {
    char                 service_name[64];
    srr_transport_type_t transport;
    char                 endpoint[128]; /* Socket path, serial device, or ip:port */
    uint32_t             baud_rate;     /* For SERIAL */
    uint16_t             service_id;    /* For SERIAL or UDP wire framing */
    uint32_t             ttl_seconds;   /* 0 = static / permanent */
    uint64_t             registered_at_ms;
    uint64_t             last_heartbeat_ms;
} srr_endpoint_entry_t;

typedef struct srr_resolver srr_resolver_t;

/*
 * ============================================================================
 * Server / Engine Lifecycle & Operations
 * ============================================================================
 */
srr_resolver_t* srr_resolver_create(void);
void            srr_resolver_destroy(srr_resolver_t *res);

int srr_resolver_register(srr_resolver_t *res, const srr_endpoint_entry_t *entry);
int srr_resolver_unregister(srr_resolver_t *res, const char *service_name);
int srr_resolver_lookup(srr_resolver_t *res, const char *service_name, srr_endpoint_entry_t *out_entry);
int srr_resolver_heartbeat(srr_resolver_t *res, const char *service_name);
int srr_resolver_list(srr_resolver_t *res, srr_endpoint_entry_t *out_entries, size_t max_entries, size_t *out_count);
void srr_resolver_prune_expired(srr_resolver_t *res, uint64_t now_ms);

/*
 * Text protocol handler for SRR request/reply transactions.
 * Supported commands:
 *   REGISTER <name> <transport> <endpoint> [baud] [service_id] [ttl]
 *   UNREGISTER <name>
 *   RESOLVE <name>
 *   HEARTBEAT <name>
 *   LIST
 */
int srr_resolver_handle_request(srr_resolver_t *res, const char *req, char *rep, size_t rep_cap);

/*
 * ============================================================================
 * Client Helper Functions (IPC / In-Process)
 * ============================================================================
 */
srr_status_t srr_resolve(
    const char *resolver_service,
    const char *service_name,
    srr_endpoint_entry_t *out_entry,
    uint32_t timeout_ms
);

srr_status_t srr_register_service(
    const char *resolver_service,
    const srr_endpoint_entry_t *entry,
    uint32_t timeout_ms
);

srr_status_t srr_unregister_service(
    const char *resolver_service,
    const char *service_name,
    uint32_t timeout_ms
);

const char* srr_transport_to_string(srr_transport_type_t t);
srr_transport_type_t srr_transport_from_string(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* SRR_RESOLVER_H */
