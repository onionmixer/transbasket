/* Occupied HTTP connection slots and the "limit reached" log rule; see
 * conn_watch.h and PLAN_CONNECTION_LIMIT.md 2.2. */
#include <string.h>
#include "conn_watch.h"

int conn_watch_init(ConnWatch *w, unsigned int limit) {
    if (!w || limit == 0) {
        return -1;
    }
    memset(w, 0, sizeof(*w));
    if (pthread_mutex_init(&w->mutex, NULL) != 0) {
        return -1;
    }
    w->limit = limit;
    return 0;
}

void conn_watch_destroy(ConnWatch *w) {
    if (w) {
        pthread_mutex_destroy(&w->mutex);
    }
}

void conn_watch_set_stopping(ConnWatch *w) {
    pthread_mutex_lock(&w->mutex);
    w->stopping = true;
    pthread_mutex_unlock(&w->mutex);
}

ConnWatchEvent conn_watch_started(ConnWatch *w, int64_t now_ms, uint64_t *unreported_out) {
    ConnWatchEvent ev = CONN_WATCH_NONE;
    pthread_mutex_lock(&w->mutex);
    w->open++;
    if (w->open == w->limit) {
        if (!w->reported_once || now_ms - w->last_report_ms >= CONN_WATCH_REPORT_INTERVAL_MS) {
            ev = CONN_WATCH_REACHED;
            if (unreported_out) {
                *unreported_out = w->unreported;
            }
            w->unreported = 0;
            w->reported_once = true;
            w->last_report_ms = now_ms;
            w->episode_reported = true;
            w->episode_start_ms = now_ms;
        } else {
            w->unreported++;
            w->episode_reported = false;
        }
    }
    pthread_mutex_unlock(&w->mutex);
    return ev;
}

ConnWatchEvent conn_watch_closed(ConnWatch *w, int64_t now_ms, int64_t *episode_ms_out, bool *stopping_out) {
    ConnWatchEvent ev = CONN_WATCH_NONE;
    pthread_mutex_lock(&w->mutex);
    if (w->open > 0) {                 /* libmicrohttpd pairs them; never wrap */
        if (w->open == w->limit && w->episode_reported) {
            ev = CONN_WATCH_BELOW;
            if (episode_ms_out) {
                *episode_ms_out = now_ms - w->episode_start_ms;
            }
            if (stopping_out) {
                *stopping_out = w->stopping;
            }
        }
        if (w->open == w->limit) {
            w->episode_reported = false;
        }
        w->open--;
    }
    pthread_mutex_unlock(&w->mutex);
    return ev;
}

unsigned int conn_watch_open(ConnWatch *w) {
    pthread_mutex_lock(&w->mutex);
    unsigned int n = w->open;
    pthread_mutex_unlock(&w->mutex);
    return n;
}
