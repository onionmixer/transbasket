#ifndef INFLIGHT_TRANSLATION_H
#define INFLIGHT_TRANSLATION_H

#include "trans_cache.h"
#include "translation_control.h"

typedef struct InflightManager InflightManager;
typedef struct InflightEntry InflightEntry;
typedef struct {
    InflightEntry *entry;
    bool owner;
} InflightHandle;

typedef struct {
    const char *text;
    const char *message;
    int status;
    bool retryable;
} InflightResult;

typedef struct {
    size_t running, waiters, live;
    unsigned long started, joined, completed;
} InflightStats;

typedef enum {
    INFLIGHT_OWNER, INFLIGHT_FOLLOWER, INFLIGHT_CACHE,
    INFLIGHT_BUSY, INFLIGHT_STOPPED, INFLIGHT_ERROR
} InflightAdmission;

InflightManager *inflight_create(size_t max_keys, size_t max_waiters,
                                size_t per_key, int timeout, int timeout_status);
/* Cache snapshot is caller-owned. Only a new owner gets a below-threshold
 * snapshot. Recheck is serialized with registration, always manager -> cache. */
InflightAdmission inflight_acquire(InflightManager *manager, TransCache *cache,
    int threshold, const char *from, const char *to, const char *text,
    InflightHandle *handle, CacheEntry **snapshot);
const TranslationControl *inflight_control(const InflightHandle *handle);
unsigned long inflight_id(const InflightHandle *handle);
/* Result strings are borrowed until release. Waiting never promotes a follower. */
InflightResult inflight_wait(InflightManager *manager, const InflightHandle *handle);
/* Takes ownership of text/message on every path. Call once after engine cleanup
 * and cache update, even if the owner's client has disconnected. */
void inflight_publish(InflightManager *manager, const InflightHandle *handle,
                     char *text, char *message, int status, bool retryable);
void inflight_release(InflightManager *manager, InflightHandle *handle);
void inflight_shutdown(InflightManager *manager);
InflightStats inflight_stats(InflightManager *manager);
/* Only after MHD has joined all handlers. */
void inflight_destroy(InflightManager *manager);
#endif
