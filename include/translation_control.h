#ifndef TRANSLATION_CONTROL_H
#define TRANSLATION_CONTROL_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

/* One monotonic deadline covers retries, queueing and I/O. The cancellation
 * flag belongs to the server and outlives every request. NULL preserves legacy
 * per-call limits for callers outside the HTTP server. */
typedef struct {
    struct timespec deadline;
    const atomic_bool *cancelled;
} TranslationControl;

static inline int64_t translation_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static inline bool translation_cancelled(const TranslationControl *control) {
    return control && control->cancelled && atomic_load(control->cancelled);
}

static inline long translation_remaining_ms(const TranslationControl *control) {
    if (!control) return INT_MAX;
    if (translation_cancelled(control)) return 0;
    int64_t left = (int64_t)control->deadline.tv_sec * 1000 +
                   control->deadline.tv_nsec / 1000000 - translation_now_ms();
    return left > INT_MAX ? INT_MAX : left > 0 ? (long)left : 0;
}

static inline bool translation_pause(const TranslationControl *control, long ms) {
    int64_t end = translation_now_ms() + ms;
    while (translation_now_ms() < end) {
        long left = translation_remaining_ms(control);
        if (!left) return false;
        long slice = (long)(end - translation_now_ms());
        if (slice > 50) slice = 50;
        if (slice > left) slice = left;
        if (slice > 0) {
            struct timespec delay = {0, slice * 1000000};
            nanosleep(&delay, NULL);
        }
    }
    return translation_remaining_ms(control) > 0;
}
#endif
