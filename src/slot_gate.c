/**
 * FIFO concurrency slots (see include/slot_gate.h, PLAN_OUT_OF_SCOPE.md C).
 */

#include <errno.h>
#include <time.h>
#include "slot_gate.h"

int slot_gate_init(SlotGate *g, int slots) {
    if (slots < 1) {
        return -1;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        return -1;
    }
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(&g->cond, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        return -1;
    }
    if (pthread_mutex_init(&g->mutex, NULL) != 0) {
        pthread_cond_destroy(&g->cond);
        return -1;
    }
    g->free_slots = slots;
    g->waiters = 0;
    g->grants = 0;
    g->joins = 0;
    g->head = g->tail = NULL;
    return 0;
}

void slot_gate_destroy(SlotGate *g) {
    pthread_cond_destroy(&g->cond);
    pthread_mutex_destroy(&g->mutex);
}

static void unlink_waiter(SlotGate *g, SlotWaiter *w) {
    if (w->prev) w->prev->next = w->next; else g->head = w->next;
    if (w->next) w->next->prev = w->prev; else g->tail = w->prev;
    w->prev = w->next = NULL;
    g->waiters--;
}

int slot_gate_acquire(SlotGate *g, int64_t deadline_ms, const TranslationControl *control) {
    return slot_gate_acquire_numbered(g, deadline_ms, control, NULL);
}

int slot_gate_acquire_numbered(SlotGate *g, int64_t deadline_ms, const TranslationControl *control,
                               uint64_t *grant_no) {
    pthread_mutex_lock(&g->mutex);
    if (!g->head && g->free_slots > 0) {          /* nobody waiting: take it */
        g->free_slots--;
        if (grant_no) *grant_no = g->grants;
        g->grants++;
        pthread_mutex_unlock(&g->mutex);
        return 0;
    }
    SlotWaiter me = { g->tail, NULL };
    if (g->tail) g->tail->next = &me; else g->head = &me;
    g->tail = &me;
    g->waiters++;
    g->joins++;
    int result = -1;
    for (;;) {
        if (g->head == &me && g->free_slots > 0) {
            unlink_waiter(g, &me);
            g->free_slots--;
            if (grant_no) *grant_no = g->grants;
            g->grants++;
            result = 0;
            break;
        }
        int64_t now = translation_now_ms();
        long left = translation_remaining_ms(control);
        if (now >= deadline_ms || !left) {
            unlink_waiter(g, &me);
            break;
        }
        /* 20 ms slices notice cancellation; never sleep past either deadline */
        int64_t until = now + 20;
        if (until > deadline_ms) until = deadline_ms;
        if (until > now + left) until = now + left;
        struct timespec ts = { (time_t)(until / 1000), (long)(until % 1000) * 1000000L };
        pthread_cond_timedwait(&g->cond, &g->mutex, &ts);       /* any wake: re-check */
        /* As translation_pause() did: once the total deadline or shutdown has hit
         * during the wait, no further attempt - the slot is not taken. The
         * waiter's own deadline_ms is looked at only after the next attempt. */
        if (!translation_remaining_ms(control)) {
            unlink_waiter(g, &me);
            break;
        }
    }
    /* Leaving (with or without a slot) may let the next head proceed. */
    if (g->head && g->free_slots > 0) {
        pthread_cond_broadcast(&g->cond);
    }
    pthread_mutex_unlock(&g->mutex);
    return result;
}

void slot_gate_release(SlotGate *g) {
    pthread_mutex_lock(&g->mutex);
    g->free_slots++;
    if (g->head) {
        pthread_cond_broadcast(&g->cond);
    }
    pthread_mutex_unlock(&g->mutex);
}

int slot_gate_free(SlotGate *g) {
    pthread_mutex_lock(&g->mutex);
    int n = g->free_slots;
    pthread_mutex_unlock(&g->mutex);
    return n;
}

int slot_gate_waiters(SlotGate *g) {
    pthread_mutex_lock(&g->mutex);
    int n = g->waiters;
    pthread_mutex_unlock(&g->mutex);
    return n;
}

uint64_t slot_gate_joins(SlotGate *g) {
    pthread_mutex_lock(&g->mutex);
    uint64_t n = g->joins;
    pthread_mutex_unlock(&g->mutex);
    return n;
}
