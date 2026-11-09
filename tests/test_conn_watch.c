/* conn_watch.c state machine with supplied monotonic times
 * (PLAN_CONNECTION_LIMIT.md 2.2, 4). `make test-conn-watch` */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "conn_watch.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); } else { printf("  FAIL  "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

static ConnWatchEvent start(ConnWatch *w, int64_t t, uint64_t *k) {
    *k = UINT64_MAX;
    return conn_watch_started(w, t, k);
}

static ConnWatchEvent close_(ConnWatch *w, int64_t t, int64_t *ms, bool *stop) {
    *ms = -1;
    *stop = false;
    return conn_watch_closed(w, t, ms, stop);
}

int main(void) {
    ConnWatch w;
    uint64_t k;
    int64_t ms;
    bool stop;

    CHECK(conn_watch_init(&w, 0) != 0, "limit 0 is refused");

    /* limit 3; t = 5 s: a freshly booted host must still report the first episode */
    conn_watch_init(&w, 3);
    CHECK(start(&w, 5000, &k) == CONN_WATCH_NONE && start(&w, 5001, &k) == CONN_WATCH_NONE,
          "below the limit: nothing (1, 2 of 3)");
    CHECK(start(&w, 5002, &k) == CONN_WATCH_REACHED && k == 0, "first episode always reported, K 0");
    CHECK(conn_watch_open(&w) == 3, "3 slots occupied");
    CHECK(close_(&w, 6502, &ms, &stop) == CONN_WATCH_BELOW && ms == 1500 && !stop,
          "end of a reported episode: BELOW after 1500 ms (%lld), not stopping", (long long)ms);
    CHECK(close_(&w, 6503, &ms, &stop) == CONN_WATCH_NONE, "3 -> 2 -> 1: one BELOW only");

    /* 1 -> 2 -> 3 again within 60 s of the report (times only go forward):
     * suppressed and silent, twice - the second exactly 59,999 ms after it */
    start(&w, 6600, &k);
    CHECK(start(&w, 30000, &k) == CONN_WATCH_NONE, "an episode 24,998 ms after the last report: not reported");
    CHECK(close_(&w, 30001, &ms, &stop) == CONN_WATCH_NONE, "an unreported episode ends silently");
    CHECK(start(&w, 5002 + 59999, &k) == CONN_WATCH_NONE, "59,999 ms after the last report: not reported");
    CHECK(close_(&w, 5002 + 59999, &ms, &stop) == CONN_WATCH_NONE, "that one ends silently too");

    /* exactly 60,000 ms after the last report: reported, with K = 2 */
    CHECK(start(&w, 5002 + 60000, &k) == CONN_WATCH_REACHED && k == 2,
          "60,000 ms after the last report: reported with K 2 (%llu)", (unsigned long long)k);
    CHECK(close_(&w, 5002 + 60000 + 250, &ms, &stop) == CONN_WATCH_BELOW && ms == 250, "its BELOW after 250 ms");
    start(&w, 5002 + 60000 + 300, &k);   /* suppressed: K 1 */
    close_(&w, 5002 + 60000 + 301, &ms, &stop);
    CHECK(start(&w, 5002 + 120000, &k) == CONN_WATCH_REACHED && k == 1,
          "K starts again from 0 after a report (now 1)");

    /* staying at the limit does not report again; stopping marks the BELOW */
    CHECK(conn_watch_open(&w) == 3, "at the limit");
    conn_watch_set_stopping(&w);
    CHECK(close_(&w, 5002 + 300000, &ms, &stop) == CONN_WATCH_BELOW && stop && ms == 180000,
          "BELOW during shutdown, 180,000 ms");
    close_(&w, 1, &ms, &stop);
    close_(&w, 1, &ms, &stop);
    CHECK(conn_watch_open(&w) == 0, "all slots back");
    CHECK(close_(&w, 1, &ms, &stop) == CONN_WATCH_NONE && conn_watch_open(&w) == 0,
          "an extra CLOSED does not wrap below 0");
    conn_watch_destroy(&w);

    /* limit 1: every connection is an episode */
    conn_watch_init(&w, 1);
    CHECK(start(&w, 0, &k) == CONN_WATCH_REACHED && k == 0, "limit 1: first connection reports");
    CHECK(close_(&w, 10, &ms, &stop) == CONN_WATCH_BELOW && ms == 10, "limit 1: BELOW after 10 ms");
    CHECK(start(&w, 20, &k) == CONN_WATCH_NONE && close_(&w, 30, &ms, &stop) == CONN_WATCH_NONE,
          "limit 1: next one within 60 s is silent");
    CHECK(start(&w, 60000, &k) == CONN_WATCH_REACHED && k == 1, "limit 1: 60 s later reported, K 1");
    conn_watch_destroy(&w);

    printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
