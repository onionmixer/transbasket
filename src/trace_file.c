/**
 * DEBUG trace files shared by both translation backends.
 * Split out of http_client.c (PLAN_BACKEND_MODULES.md step 1); the code below is moved verbatim.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdatomic.h>
#include "http_client.h"
#include "codex_exec.h"
#include "strict_json.h"
#include "source_hint.h"
#include "language_table.h"
#include "utils.h"
#include "trace_file.h"

/* Open a new trace file ./trace/<base><ext> for writing (DEBUG=yes only;
 * PLAN_OPEN_ISSUES.md B). The directory is created 0700 if missing; an existing one
 * with group/other permissions is used but warned about once (its permissions are
 * the operator's). Files are created 0600 with O_EXCL|O_NOFOLLOW, never reusing or
 * following an existing name: a clash (same client uuid+timestamp) gets "-1".."-9". */
FILE *open_trace_file(const char *base, const char *ext, char *path, size_t path_size) {
    static atomic_bool warned = false;
    const char *dir = "./trace";
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            LOG_DEBUG("Warning: Failed to create trace directory: %s\n", dir);
            return NULL;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return NULL;
    } else if ((st.st_mode & 077) && !atomic_exchange(&warned, true)) {
        LOG_INFO("Warning: %s is accessible to group/others (mode %03o); trace files hold request text\n",
                 dir, (unsigned)(st.st_mode & 0777));
    }
    for (int k = 0; k < 10; k++) {
        if (k == 0) {
            snprintf(path, path_size, "%s/%s%s", dir, base, ext);
        } else {
            snprintf(path, path_size, "%s/%s-%d%s", dir, base, k, ext);
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            FILE *fp = fdopen(fd, "w");
            if (!fp) {
                close(fd);
            }
            return fp;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }
    return NULL;
}

/* Trace file base name from the request's timestamp and uuid (both validated), ':' -> '-'. */
void trace_base(char *out, size_t size, const char *timestamp, const char *uuid) {
    snprintf(out, size, "%s_%s", timestamp, uuid);
    for (char *q = out; *q; q++) {
        if (*q == ':') *q = '-';
    }
}
