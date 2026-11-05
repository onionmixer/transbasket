#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "inflight_translation.h"

struct InflightEntry {
    char hash[65];
    char *from, *to, *source;
    pthread_cond_t changed;
    size_t refs, waiters;
    unsigned long id;
    bool done;
    char *text, *message;
    int status;
    bool retryable;
    TranslationControl control;
    struct InflightEntry *next;
};

struct InflightManager {
    pthread_mutex_t mutex;
    atomic_bool stopping;
    InflightEntry *head;
    size_t max_keys, max_waiters, per_key;
    int timeout, timeout_status;
    InflightStats stats;
};

InflightManager *inflight_create(size_t max_keys, size_t max_waiters,
                                size_t per_key, int timeout, int timeout_status) {
    if (!max_keys || !max_waiters || !per_key || timeout < 1) return NULL;
    InflightManager *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->mutex, NULL) != 0) { free(m); return NULL; }
    atomic_init(&m->stopping, false);
    m->max_keys = max_keys; m->max_waiters = max_waiters; m->per_key = per_key;
    m->timeout = timeout; m->timeout_status = timeout_status;
    return m;
}

InflightAdmission inflight_acquire(InflightManager *m, TransCache *cache,
    int threshold, const char *from, const char *to, const char *text,
    InflightHandle *handle, CacheEntry **snapshot) {
    *handle = (InflightHandle){0};
    *snapshot = NULL;
    char hash[65];
    trans_cache_calculate_hash(from, to, text, hash);
    if (!*hash) return INFLIGHT_ERROR;
    pthread_mutex_lock(&m->mutex);
    InflightAdmission admission = INFLIGHT_ERROR;
    if (atomic_load(&m->stopping)) { admission = INFLIGHT_STOPPED; goto out; }
    for (InflightEntry *e = m->head; e; e = e->next) {
        if (strcmp(e->hash, hash) || strcmp(e->from, from) ||
            strcmp(e->to, to) || strcmp(e->source, text)) continue;
        if (m->stats.waiters >= m->max_waiters || e->waiters >= m->per_key) {
            admission = INFLIGHT_BUSY; goto out;
        }
        e->refs++; e->waiters++; m->stats.waiters++; m->stats.joined++;
        *handle = (InflightHandle){e, false};
        LOG_INFO("Translation group %lu joined (waiters=%zu)", e->id, e->waiters);
        admission = INFLIGHT_FOLLOWER;
        goto out;
    }
    /* Never hold a cache lock when entering this function. */
    if (cache) *snapshot = trans_cache_lookup(cache, from, to, text);
    if (*snapshot && (*snapshot)->count >= threshold) { admission = INFLIGHT_CACHE; goto out; }
    if (m->stats.running >= m->max_keys) { admission = INFLIGHT_BUSY; goto out; }

    InflightEntry *e = calloc(1, sizeof(*e));
    if (!e) goto out;
    e->from = strdup(from); e->to = strdup(to); e->source = strdup(text);
    if (!e->from || !e->to || !e->source) goto fail_entry;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) goto fail_entry;
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_cond_init(&e->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (rc) goto fail_entry;
    memcpy(e->hash, hash, sizeof(hash));
    clock_gettime(CLOCK_MONOTONIC, &e->control.deadline);
    e->control.deadline.tv_sec += m->timeout;
    e->control.cancelled = &m->stopping;
    e->refs = 2; /* registry + owner */
    e->id = ++m->stats.started;
    e->next = m->head; m->head = e;
    m->stats.running++; m->stats.live++;
    *handle = (InflightHandle){e, true};
    LOG_INFO("Translation group %lu started (running=%zu)", e->id, m->stats.running);
    admission = INFLIGHT_OWNER;
    goto out;
fail_entry:
    free(e->from); free(e->to); free(e->source); free(e);
out:
    pthread_mutex_unlock(&m->mutex);
    return admission;
}

const TranslationControl *inflight_control(const InflightHandle *handle) {
    return &handle->entry->control;
}

unsigned long inflight_id(const InflightHandle *handle) {
    return handle->entry->id;
}

InflightResult inflight_wait(InflightManager *m, const InflightHandle *handle) {
    InflightEntry *e = handle->entry;
    pthread_mutex_lock(&m->mutex);
    while (!e->done && !atomic_load(&m->stopping)) {
        int rc = pthread_cond_timedwait(&e->changed, &m->mutex, &e->control.deadline);
        if (rc && rc != EINTR) break; /* includes timeout; still own our reference */
    }
    InflightResult result;
    if (e->done) {
        result = (InflightResult){e->text, e->message ? e->message : "Translation failed", e->status, e->retryable};
    } else if (atomic_load(&m->stopping)) {
        result = (InflightResult){NULL, "Server shutting down", 503, true};
    } else {
        result = (InflightResult){NULL, "Translation deadline exceeded", m->timeout_status, true};
    }
    pthread_mutex_unlock(&m->mutex);
    return result;
}

void inflight_publish(InflightManager *m, const InflightHandle *handle,
                     char *text, char *message, int status, bool retryable) {
    InflightEntry *e = handle->entry;
    pthread_mutex_lock(&m->mutex);
    assert(handle->owner && !e->done);
    if (!translation_remaining_ms(&e->control)) {
        free(text); free(message); text = NULL;
        bool cancelled = atomic_load(&m->stopping);
        message = strdup(cancelled ? "Server shutting down" : "Translation deadline exceeded");
        status = cancelled ? 503 : m->timeout_status; retryable = true;
    }
    e->text = text; e->message = message; e->status = status; e->retryable = retryable;
    e->done = true;
    InflightEntry **link = &m->head;
    while (*link && *link != e) link = &(*link)->next;
    assert(*link == e);
    *link = e->next;
    e->refs--; m->stats.running--; m->stats.completed++;
    pthread_cond_broadcast(&e->changed);
    LOG_INFO("Translation group %lu flushed (status=%d, waiters=%zu)", e->id, status, e->waiters);
    pthread_mutex_unlock(&m->mutex);
}

void inflight_release(InflightManager *m, InflightHandle *handle) {
    if (!handle->entry) return;
    pthread_mutex_lock(&m->mutex);
    InflightEntry *e = handle->entry;
    if (!handle->owner) { e->waiters--; m->stats.waiters--; }
    if (--e->refs == 0) {
        assert(e->done);
        pthread_cond_destroy(&e->changed);
        free(e->from); free(e->to); free(e->source);
        free(e->text); free(e->message); free(e);
        m->stats.live--;
    }
    *handle = (InflightHandle){0};
    pthread_mutex_unlock(&m->mutex);
}

void inflight_shutdown(InflightManager *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mutex);
    atomic_store(&m->stopping, true);
    for (InflightEntry *e = m->head; e; e = e->next) pthread_cond_broadcast(&e->changed);
    pthread_mutex_unlock(&m->mutex);
}

InflightStats inflight_stats(InflightManager *m) {
    pthread_mutex_lock(&m->mutex);
    InflightStats stats = m->stats;
    pthread_mutex_unlock(&m->mutex);
    return stats;
}

void inflight_destroy(InflightManager *m) {
    if (!m) return;
    assert(!m->stats.running && !m->stats.waiters && !m->stats.live);
    LOG_INFO("Translation groups drained (running=0, waiters=0, live=0, started=%lu, joined=%lu)",
             m->stats.started, m->stats.joined);
    pthread_mutex_destroy(&m->mutex);
    free(m);
}
