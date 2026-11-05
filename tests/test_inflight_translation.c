#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "inflight_translation.h"

/* Targeted allocation failures without production-only injection switches. */
static bool fail_strdup;
char *__real_strdup(const char *text);
char *__wrap_strdup(const char *text) {
    if (fail_strdup) { fail_strdup = false; return NULL; }
    return __real_strdup(text);
}

typedef struct {
    InflightManager *manager;
    pthread_barrier_t *barrier;
    bool stopping;
} Waiter;

static InflightAdmission acquire(InflightManager *m, const char *source, InflightHandle *h) {
    CacheEntry *cache = NULL;
    InflightAdmission result = inflight_acquire(m, NULL, 3, "auto", "kor", source, h, &cache);
    assert(!cache);
    return result;
}

static void drained(InflightManager *m) {
    InflightStats stats = inflight_stats(m);
    assert(stats.running == 0 && stats.waiters == 0 && stats.live == 0);
}

static void *waiter(void *argument) {
    Waiter *w = argument;
    InflightHandle h;
    assert(acquire(w->manager, "same", &h) == INFLIGHT_FOLLOWER);
    pthread_barrier_wait(w->barrier);
    InflightResult result = inflight_wait(w->manager, &h);
    if (w->stopping) assert(result.status == 503 && !result.text);
    else assert(result.status == 200 && strcmp(result.text, "shared") == 0);
    inflight_release(w->manager, &h);
    return NULL;
}

static void test_broadcast(bool stopping) {
    enum { N = 32 };
    InflightManager *m = inflight_create(2, N, N, 5, 503);
    assert(m);
    InflightHandle owner;
    assert(acquire(m, "same", &owner) == INFLIGHT_OWNER);
    pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, N + 1));
    Waiter w = {m, &barrier, stopping};
    pthread_t threads[N];
    for (int i = 0; i < N; i++) assert(!pthread_create(&threads[i], NULL, waiter, &w));
    pthread_barrier_wait(&barrier);
    assert(inflight_stats(m).waiters == N);
    if (stopping) inflight_shutdown(m);
    inflight_publish(m, &owner, strdup("shared"), NULL, 200, false);
    inflight_release(m, &owner);
    for (int i = 0; i < N; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&barrier);
    assert(inflight_stats(m).started == 1 && inflight_stats(m).joined == N);
    drained(m);
    inflight_destroy(m);
}

static void test_capacity_and_failure(void) {
    InflightManager *m = inflight_create(2, 2, 1, 5, 503);
    InflightHandle a, b, a_wait, b_wait, rejected;
    assert(acquire(m, "a", &a) == INFLIGHT_OWNER);
    assert(acquire(m, "b", &b) == INFLIGHT_OWNER);
    assert(acquire(m, "c", &rejected) == INFLIGHT_BUSY);
    assert(acquire(m, "a", &a_wait) == INFLIGHT_FOLLOWER); /* full keys still allow joins */
    assert(acquire(m, "a", &rejected) == INFLIGHT_BUSY);
    assert(acquire(m, "b", &b_wait) == INFLIGHT_FOLLOWER);
    inflight_publish(m, &a, NULL, NULL, 502, false); /* error allocation may fail */
    InflightResult result = inflight_wait(m, &a_wait);
    assert(result.status == 502 && !result.retryable && result.message);
    inflight_release(m, &a); inflight_release(m, &a_wait);
    assert(acquire(m, "a", &a) == INFLIGHT_OWNER); /* failures are not retained */
    inflight_publish(m, &a, NULL, strdup("again"), 503, true);
    inflight_release(m, &a);
    inflight_publish(m, &b, strdup("done"), NULL, 200, false);
    inflight_release(m, &b); inflight_release(m, &b_wait);
    drained(m);
    fail_strdup = true;
    assert(acquire(m, "allocation", &a) == INFLIGHT_ERROR);
    drained(m);
    assert(acquire(m, "allocation", &a) == INFLIGHT_OWNER);
    inflight_shutdown(m);
    assert(acquire(m, "new", &rejected) == INFLIGHT_STOPPED);
    inflight_publish(m, &a, NULL, NULL, 503, true);
    inflight_release(m, &a);
    drained(m);
    inflight_destroy(m);
}

static void test_timeout_does_not_promote(void) {
    InflightManager *m = inflight_create(1, 2, 2, 1, 504);
    InflightHandle owner, follower, late;
    assert(acquire(m, "slow", &owner) == INFLIGHT_OWNER);
    assert(acquire(m, "slow", &follower) == INFLIGHT_FOLLOWER);
    InflightResult result = inflight_wait(m, &follower);
    assert(result.status == 504 && result.retryable);
    inflight_release(m, &follower);
    assert(acquire(m, "slow", &late) == INFLIGHT_FOLLOWER);
    assert(inflight_stats(m).started == 1 && inflight_stats(m).running == 1);
    inflight_release(m, &late);
    inflight_publish(m, &owner, strdup("too late"), NULL, 200, false);
    result = inflight_wait(m, &owner);
    assert(!result.text && result.status == 504);
    inflight_release(m, &owner);
    drained(m);
    inflight_destroy(m);
}

static void test_cache_recheck_and_global_limit(void) {
    InflightManager *m = inflight_create(1, 1, 2, 5, 503);
    InflightHandle owner, follower, extra;
    assert(acquire(m, "busy", &owner) == INFLIGHT_OWNER);
    assert(acquire(m, "busy", &follower) == INFLIGHT_FOLLOWER);
    assert(acquire(m, "busy", &extra) == INFLIGHT_BUSY); /* global limit before per-key limit */
    char path[] = "/tmp/inflight-cache-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    TransCache *cache = trans_cache_init(path);
    assert(cache && trans_cache_add(cache, "auto", "kor", "cached", "ready") == 0);
    CacheEntry *snapshot = NULL;
    /* Simulate a cache becoming eligible between fast lookup and registration.
     * It must win even when the unique-key admission capacity is exhausted. */
    assert(inflight_acquire(m, cache, 1, "auto", "kor", "cached", &extra, &snapshot) == INFLIGHT_CACHE);
    assert(!extra.entry && snapshot && strcmp(snapshot->translated_text, "ready") == 0);
    trans_cache_entry_free(snapshot);
    trans_cache_free(cache);
    unlink(path);
    inflight_publish(m, &owner, strdup("done"), NULL, 200, false);
    inflight_release(m, &owner); inflight_release(m, &follower);
    drained(m);
    inflight_destroy(m);
}

int main(void) {
    test_broadcast(false);
    test_broadcast(true);
    test_capacity_and_failure();
    test_timeout_does_not_promote();
    test_cache_recheck_and_global_limit();
    puts("inflight module tests passed");
    return 0;
}
