#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "srr_resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>

struct srr_resolver {
    pthread_mutex_t      lock;
    srr_endpoint_entry_t entries[SRR_RESOLVER_MAX_ENTRIES];
    size_t               count;
};

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

const char* srr_transport_to_string(srr_transport_type_t t) {
    switch (t) {
        case SRR_TRANSPORT_INPROC: return "INPROC";
        case SRR_TRANSPORT_IPC:    return "IPC";
        case SRR_TRANSPORT_SERIAL: return "SERIAL";
        case SRR_TRANSPORT_UDP:    return "UDP";
        default:                   return "UNKNOWN";
    }
}

srr_transport_type_t srr_transport_from_string(const char *s) {
    if (!s) return SRR_TRANSPORT_IPC;
    if (strcasecmp(s, "INPROC") == 0) return SRR_TRANSPORT_INPROC;
    if (strcasecmp(s, "IPC") == 0)    return SRR_TRANSPORT_IPC;
    if (strcasecmp(s, "SERIAL") == 0) return SRR_TRANSPORT_SERIAL;
    if (strcasecmp(s, "UDP") == 0)    return SRR_TRANSPORT_UDP;
    return SRR_TRANSPORT_IPC;
}

srr_resolver_t* srr_resolver_create(void) {
    srr_resolver_t *res = (srr_resolver_t *)calloc(1, sizeof(srr_resolver_t));
    if (!res) return NULL;
    pthread_mutex_init(&res->lock, NULL);
    return res;
}

void srr_resolver_destroy(srr_resolver_t *res) {
    if (!res) return;
    pthread_mutex_destroy(&res->lock);
    free(res);
}

void srr_resolver_prune_expired(srr_resolver_t *res, uint64_t now_ms) {
    if (!res) return;
    pthread_mutex_lock(&res->lock);
    size_t i = 0;
    while (i < res->count) {
        srr_endpoint_entry_t *e = &res->entries[i];
        if (e->ttl_seconds > 0) {
            uint64_t expire_at = e->last_heartbeat_ms + ((uint64_t)e->ttl_seconds * 1000ULL);
            if (now_ms > expire_at) {
                /* Remove by swapping with last entry */
                res->entries[i] = res->entries[res->count - 1];
                res->count--;
                continue;
            }
        }
        i++;
    }
    pthread_mutex_unlock(&res->lock);
}

int srr_resolver_register(srr_resolver_t *res, const srr_endpoint_entry_t *entry) {
    if (!res || !entry || entry->service_name[0] == '\0') return -1;
    uint64_t now = current_time_ms();

    pthread_mutex_lock(&res->lock);
    /* Check if already exists to update */
    for (size_t i = 0; i < res->count; i++) {
        if (strncmp(res->entries[i].service_name, entry->service_name, sizeof(res->entries[i].service_name)) == 0) {
            res->entries[i] = *entry;
            res->entries[i].registered_at_ms = now;
            res->entries[i].last_heartbeat_ms = now;
            pthread_mutex_unlock(&res->lock);
            return 0;
        }
    }

    if (res->count >= SRR_RESOLVER_MAX_ENTRIES) {
        pthread_mutex_unlock(&res->lock);
        return -1; /* Table full */
    }

    res->entries[res->count] = *entry;
    res->entries[res->count].registered_at_ms = now;
    res->entries[res->count].last_heartbeat_ms = now;
    res->count++;
    pthread_mutex_unlock(&res->lock);
    return 0;
}

int srr_resolver_unregister(srr_resolver_t *res, const char *service_name) {
    if (!res || !service_name) return -1;
    pthread_mutex_lock(&res->lock);
    for (size_t i = 0; i < res->count; i++) {
        if (strncmp(res->entries[i].service_name, service_name, sizeof(res->entries[i].service_name)) == 0) {
            res->entries[i] = res->entries[res->count - 1];
            res->count--;
            pthread_mutex_unlock(&res->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&res->lock);
    return -1;
}

int srr_resolver_lookup(srr_resolver_t *res, const char *service_name, srr_endpoint_entry_t *out_entry) {
    if (!res || !service_name || !out_entry) return -1;
    uint64_t now = current_time_ms();

    pthread_mutex_lock(&res->lock);
    for (size_t i = 0; i < res->count; i++) {
        srr_endpoint_entry_t *e = &res->entries[i];
        if (strncmp(e->service_name, service_name, sizeof(e->service_name)) == 0) {
            if (e->ttl_seconds > 0) {
                uint64_t expire_at = e->last_heartbeat_ms + ((uint64_t)e->ttl_seconds * 1000ULL);
                if (now > expire_at) {
                    /* Expired */
                    res->entries[i] = res->entries[res->count - 1];
                    res->count--;
                    pthread_mutex_unlock(&res->lock);
                    return -1;
                }
            }
            *out_entry = *e;
            pthread_mutex_unlock(&res->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&res->lock);
    return -1;
}

int srr_resolver_heartbeat(srr_resolver_t *res, const char *service_name) {
    if (!res || !service_name) return -1;
    uint64_t now = current_time_ms();
    pthread_mutex_lock(&res->lock);
    for (size_t i = 0; i < res->count; i++) {
        if (strncmp(res->entries[i].service_name, service_name, sizeof(res->entries[i].service_name)) == 0) {
            res->entries[i].last_heartbeat_ms = now;
            pthread_mutex_unlock(&res->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&res->lock);
    return -1;
}

int srr_resolver_list(srr_resolver_t *res, srr_endpoint_entry_t *out_entries, size_t max_entries, size_t *out_count) {
    if (!res || !out_entries || !out_count) return -1;
    uint64_t now = current_time_ms();

    pthread_mutex_lock(&res->lock);
    size_t copied = 0;
    for (size_t i = 0; i < res->count && copied < max_entries; i++) {
        srr_endpoint_entry_t *e = &res->entries[i];
        if (e->ttl_seconds > 0) {
            uint64_t expire_at = e->last_heartbeat_ms + ((uint64_t)e->ttl_seconds * 1000ULL);
            if (now > expire_at) continue;
        }
        out_entries[copied++] = *e;
    }
    *out_count = copied;
    pthread_mutex_unlock(&res->lock);
    return 0;
}

int srr_resolver_handle_request(srr_resolver_t *res, const char *req, char *rep, size_t rep_cap) {
    if (!res || !req || !rep || rep_cap == 0) return -1;

    char cmd[32] = {0};
    if (sscanf(req, "%31s", cmd) != 1) {
        snprintf(rep, rep_cap, "ERR empty_command");
        return 0;
    }

    if (strcasecmp(cmd, "REGISTER") == 0) {
        srr_endpoint_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        char trans_str[32] = {0};
        unsigned int baud = 0, svc_id = 0, ttl = 0;

        int matched = sscanf(req, "%*s %63s %31s %127s %u %u %u",
                             entry.service_name, trans_str, entry.endpoint,
                             &baud, &svc_id, &ttl);
        if (matched < 3) {
            snprintf(rep, rep_cap, "ERR usage: REGISTER <name> <transport> <endpoint> [baud] [service_id] [ttl]");
            return 0;
        }
        entry.transport = srr_transport_from_string(trans_str);
        entry.baud_rate = baud;
        entry.service_id = (uint16_t)svc_id;
        entry.ttl_seconds = ttl;

        if (srr_resolver_register(res, &entry) == 0) {
            snprintf(rep, rep_cap, "OK registered %s %s %u", entry.service_name,
                     srr_transport_to_string(entry.transport), entry.ttl_seconds);
        } else {
            snprintf(rep, rep_cap, "ERR register_failed");
        }
        return 0;
    }

    if (strcasecmp(cmd, "UNREGISTER") == 0) {
        char name[64] = {0};
        if (sscanf(req, "%*s %63s", name) != 1) {
            snprintf(rep, rep_cap, "ERR usage: UNREGISTER <name>");
            return 0;
        }
        if (srr_resolver_unregister(res, name) == 0) {
            snprintf(rep, rep_cap, "OK unregistered %s", name);
        } else {
            snprintf(rep, rep_cap, "ERR not_found %s", name);
        }
        return 0;
    }

    if (strcasecmp(cmd, "RESOLVE") == 0) {
        char name[64] = {0};
        if (sscanf(req, "%*s %63s", name) != 1) {
            snprintf(rep, rep_cap, "ERR usage: RESOLVE <name>");
            return 0;
        }
        srr_endpoint_entry_t entry;
        if (srr_resolver_lookup(res, name, &entry) == 0) {
            snprintf(rep, rep_cap, "FOUND %s %s %s %u %u %u",
                     entry.service_name,
                     srr_transport_to_string(entry.transport),
                     entry.endpoint,
                     entry.baud_rate,
                     (unsigned int)entry.service_id,
                     entry.ttl_seconds);
        } else {
            snprintf(rep, rep_cap, "NOT_FOUND %s", name);
        }
        return 0;
    }

    if (strcasecmp(cmd, "HEARTBEAT") == 0) {
        char name[64] = {0};
        if (sscanf(req, "%*s %63s", name) != 1) {
            snprintf(rep, rep_cap, "ERR usage: HEARTBEAT <name>");
            return 0;
        }
        if (srr_resolver_heartbeat(res, name) == 0) {
            snprintf(rep, rep_cap, "OK heartbeat %s", name);
        } else {
            snprintf(rep, rep_cap, "NOT_FOUND %s", name);
        }
        return 0;
    }

    if (strcasecmp(cmd, "LIST") == 0) {
        srr_endpoint_entry_t entries[SRR_RESOLVER_MAX_ENTRIES];
        size_t count = 0;
        srr_resolver_list(res, entries, SRR_RESOLVER_MAX_ENTRIES, &count);

        size_t offset = 0;
        offset += (size_t)snprintf(rep + offset, rep_cap - offset, "COUNT %zu\n", count);
        for (size_t i = 0; i < count && offset < rep_cap; i++) {
            offset += (size_t)snprintf(rep + offset, rep_cap - offset,
                                       "ENTRY %s %s %s %u %u %u\n",
                                       entries[i].service_name,
                                       srr_transport_to_string(entries[i].transport),
                                       entries[i].endpoint,
                                       entries[i].baud_rate,
                                       (unsigned int)entries[i].service_id,
                                       entries[i].ttl_seconds);
        }
        return 0;
    }

    snprintf(rep, rep_cap, "ERR unknown_command: %s", cmd);
    return 0;
}

srr_status_t srr_resolve(
    const char *resolver_service,
    const char *service_name,
    srr_endpoint_entry_t *out_entry,
    uint32_t timeout_ms
) {
    if (!service_name || !out_entry) return SRR_INVALID;
    const char *svc = resolver_service ? resolver_service : SRR_RESOLVER_DEFAULT_SERVICE;

    srr_connection_t *conn = NULL;
    srr_status_t st = srr_connect(svc, NULL, &conn);
    if (st != SRR_OK) return st;

    char req[128];
    snprintf(req, sizeof(req), "RESOLVE %s", service_name);

    char rep[256];
    size_t rep_len = 0;
    st = srr_send_timed(conn, req, strlen(req) + 1, rep, sizeof(rep), &rep_len, timeout_ms);
    srr_disconnect(conn);
    if (st != SRR_OK) return st;

    char tag[32] = {0};
    char trans_str[32] = {0};
    unsigned int baud = 0, svc_id = 0, ttl = 0;

    int matched = sscanf(rep, "%31s %63s %31s %127s %u %u %u",
                         tag, out_entry->service_name, trans_str, out_entry->endpoint,
                         &baud, &svc_id, &ttl);
    if (matched >= 4 && strcmp(tag, "FOUND") == 0) {
        out_entry->transport = srr_transport_from_string(trans_str);
        out_entry->baud_rate = baud;
        out_entry->service_id = (uint16_t)svc_id;
        out_entry->ttl_seconds = ttl;
        return SRR_OK;
    }

    return SRR_PROTOCOL_ERROR;
}

srr_status_t srr_register_service(
    const char *resolver_service,
    const srr_endpoint_entry_t *entry,
    uint32_t timeout_ms
) {
    if (!entry) return SRR_INVALID;
    const char *svc = resolver_service ? resolver_service : SRR_RESOLVER_DEFAULT_SERVICE;

    srr_connection_t *conn = NULL;
    srr_status_t st = srr_connect(svc, NULL, &conn);
    if (st != SRR_OK) return st;

    char req[256];
    snprintf(req, sizeof(req), "REGISTER %s %s %s %u %u %u",
             entry->service_name,
             srr_transport_to_string(entry->transport),
             entry->endpoint,
             entry->baud_rate,
             (unsigned int)entry->service_id,
             entry->ttl_seconds);

    char rep[128];
    size_t rep_len = 0;
    st = srr_send_timed(conn, req, strlen(req) + 1, rep, sizeof(rep), &rep_len, timeout_ms);
    srr_disconnect(conn);
    if (st != SRR_OK) return st;

    if (strncmp(rep, "OK", 2) == 0) return SRR_OK;
    return SRR_PROTOCOL_ERROR;
}

srr_status_t srr_unregister_service(
    const char *resolver_service,
    const char *service_name,
    uint32_t timeout_ms
) {
    if (!service_name) return SRR_INVALID;
    const char *svc = resolver_service ? resolver_service : SRR_RESOLVER_DEFAULT_SERVICE;

    srr_connection_t *conn = NULL;
    srr_status_t st = srr_connect(svc, NULL, &conn);
    if (st != SRR_OK) return st;

    char req[128];
    snprintf(req, sizeof(req), "UNREGISTER %s", service_name);

    char rep[128];
    size_t rep_len = 0;
    st = srr_send_timed(conn, req, strlen(req) + 1, rep, sizeof(rep), &rep_len, timeout_ms);
    srr_disconnect(conn);
    if (st != SRR_OK) return st;

    if (strncmp(rep, "OK", 2) == 0) return SRR_OK;
    return SRR_PROTOCOL_ERROR;
}
