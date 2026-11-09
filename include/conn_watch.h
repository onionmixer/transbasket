#ifndef CONN_WATCH_H
#define CONN_WATCH_H

/* Occupied HTTP connection slots and the "limit reached" log rule
 * (PLAN_CONNECTION_LIMIT.md 2.2). libmicrohttpd reports each connection through
 * MHD_OPTION_NOTIFY_CONNECTION: STARTED right after it counted the connection,
 * CLOSED during cleanup, just before it gives the slot back. In between the
 * connection holds one of the `limit` slots - also while it waits for cleanup.
 * Once all slots are taken, new connections wait in the kernel listen backlog.
 *
 * Rule (all under one mutex, logging is left to the caller):
 *  - open goes limit-1 -> limit: the first time always, later only if the last
 *    reported one is at least CONN_WATCH_REPORT_INTERVAL_MS old, this is a
 *    reported episode (CONN_WATCH_REACHED, with the number of unreported
 *    episodes since the last report, which then starts again from 0);
 *    otherwise it only counts as unreported.
 *  - open goes limit -> limit-1 at the end of a reported episode:
 *    CONN_WATCH_BELOW with the episode's length and whether the server was
 *    stopping. An unreported episode ends silently, so the lines always pair.
 *  - Times are milliseconds of one monotonic clock, supplied by the caller. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define CONN_WATCH_REPORT_INTERVAL_MS 60000

typedef enum {
    CONN_WATCH_NONE = 0,
    CONN_WATCH_REACHED,
    CONN_WATCH_BELOW
} ConnWatchEvent;

typedef struct {
    pthread_mutex_t mutex;
    unsigned int limit;          /* effective connection limit, > 0 */
    unsigned int open;           /* occupied slots */
    bool reported_once;          /* a REACHED was returned before */
    int64_t last_report_ms;      /* time of that REACHED */
    bool episode_reported;       /* the current episode at the limit was reported */
    int64_t episode_start_ms;
    uint64_t unreported;         /* episodes at the limit since the last report */
    bool stopping;
} ConnWatch;

/* 0 on success. limit must be > 0. */
int conn_watch_init(ConnWatch *w, unsigned int limit);
void conn_watch_destroy(ConnWatch *w);

/* The server is stopping: a BELOW from now on says so. */
void conn_watch_set_stopping(ConnWatch *w);

/* A connection took a slot. REACHED sets *unreported_out (episodes not
 * reported since the previous REACHED). */
ConnWatchEvent conn_watch_started(ConnWatch *w, int64_t now_ms, uint64_t *unreported_out);

/* A connection gave its slot back. BELOW sets *episode_ms_out and *stopping_out. */
ConnWatchEvent conn_watch_closed(ConnWatch *w, int64_t now_ms, int64_t *episode_ms_out, bool *stopping_out);

/* Occupied slots now (tests). */
unsigned int conn_watch_open(ConnWatch *w);

#endif /* CONN_WATCH_H */
