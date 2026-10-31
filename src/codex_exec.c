/*
 * `codex exec` driver for the codex translation backend.
 * See include/codex_exec.h for the public contract.
 */
#define _GNU_SOURCE   /* posix_spawn_file_actions_addclosefrom_np, environ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <spawn.h>
#include <limits.h>
#include <sys/wait.h>

#include <cjson/cJSON.h>

#include "codex_exec.h"
#include "utils.h"

extern char **environ;

#define CODEX_OUT_MAX        (4 * 1024 * 1024)  /* same cap as MCP_RBUF_MAX */
#define CODEX_ERR_TAIL_MAX   16384              /* stderr kept for diagnostics */
#define CODEX_POLL_SLICE_MS  1000
#define CODEX_DRAIN_READS    16   /* reads per fd per poll round, so a chatty child
                                     cannot keep us away from the deadline check */
#define CODEX_TOKEN_MAX      64   /* see config_loader.c:is_codex_token() */
#define CODEX_PROBE_TIMEOUT  15
#define CODEX_LAST_MSG_FILE  "last-message.txt"

/* `codex mcp-server` was removed in codex-cli 0.154.0. */
#define CODEX_MCP_REMOVED_MAJOR 0
#define CODEX_MCP_REMOVED_MINOR 154

/* ---- small helpers -------------------------------------------------------- */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_fd_flags(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl != -1) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    fl = fcntl(fd, F_GETFL);
    if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Append to a growable buffer, failing once it would exceed `max`. */
static int buf_append(char **buf, size_t *len, size_t *cap, size_t max,
                      const char *data, size_t n) {
    if (*len + n + 1 > max) return -1;
    if (*len + n + 1 > *cap) {
        size_t newcap = *cap ? *cap : 8192;
        while (newcap < *len + n + 1) newcap *= 2;
        if (newcap > max) newcap = max;
        char *p = realloc(*buf, newcap);
        if (!p) return -1;
        *buf = p;
        *cap = newcap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

/* Keep only the last CODEX_ERR_TAIL_MAX bytes: codex prints its banner and the
 * echoed prompt first, and the errors that matter last. */
static void tail_append(char *buf, size_t *len, const char *data, size_t n) {
    if (n > CODEX_ERR_TAIL_MAX) {
        data += n - CODEX_ERR_TAIL_MAX;
        n = CODEX_ERR_TAIL_MAX;
    }
    if (*len + n > CODEX_ERR_TAIL_MAX) {
        size_t drop = *len + n - CODEX_ERR_TAIL_MAX;
        memmove(buf, buf + drop, *len - drop);
        *len -= drop;
    }
    memcpy(buf + *len, data, n);
    *len += n;
    buf[*len] = '\0';
}

/* Spawn argv[0] with piped stdio in its own process group (the `codex` node
 * wrapper runs the real binary as a grandchild, so we signal the whole group).
 * Same hygiene as mcp_client_start(): fds >= 3 closed, signal state reset. */
static int spawn_child(char *const argv[], pid_t *pid, int *fd_in, int *fd_out, int *fd_err) {
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 }, err_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        LOG_INFO("Error: codex exec pipe() failed: %s", strerror(errno));
        goto fail;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);

    sigset_t empty, all;
    sigemptyset(&empty);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                    POSIX_SPAWN_SETPGROUP);

    int rc = posix_spawnp(pid, argv[0], &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        LOG_INFO("Error: posix_spawnp(%s) failed: %s", argv[0], strerror(rc));
        goto fail;
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    *fd_in = in_pipe[1];
    *fd_out = out_pipe[0];
    *fd_err = err_pipe[0];
    set_fd_flags(*fd_in);
    set_fd_flags(*fd_out);
    set_fd_flags(*fd_err);
    return 0;

fail:
    for (int i = 0; i < 2; i++) {
        if (in_pipe[i] >= 0) close(in_pipe[i]);
        if (out_pipe[i] >= 0) close(out_pipe[i]);
        if (err_pipe[i] >= 0) close(err_pipe[i]);
    }
    return -1;
}

/* Wait until the group leader has exited, WITHOUT reaping it (WNOWAIT): while
 * it is a zombie its pid - which is also our pgid - cannot be reused, so
 * signalling the group stays safe. Returns 1 exited, 0 deadline, -1 error. */
static int wait_leader_exit(pid_t pid, long deadline) {
    for (;;) {
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (si.si_pid == pid) return 1;
        } else if (errno != EINTR) {
            return -1;
        }
        if (now_ms() >= deadline) return 0;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

/* Reap the child. `abort_now`: SIGTERM the group at once (timeout/overflow).
 * Otherwise wait for a normal exit until `deadline` (the request deadline) and
 * set *expired if it was missed. Either way the whole group gets SIGKILL before
 * the leader is reaped, so a descendant that ignored SIGTERM, or outlived a
 * leader that exited, does not survive the request. Returns the waitpid status
 * (-1 if it could not be collected). */
static int reap_child(pid_t pid, long deadline, bool abort_now, bool *expired) {
    if (!abort_now && wait_leader_exit(pid, deadline) == 0) {
        *expired = true;
        abort_now = true;
    }
    if (abort_now) {
        kill(-pid, SIGTERM);
        wait_leader_exit(pid, now_ms() + 2000);
    }
    kill(-pid, SIGKILL);

    int status = -1;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}

typedef struct {
    char  *out;
    size_t out_len, out_cap;
    char   err[CODEX_ERR_TAIL_MAX + 1];
    size_t err_len;
    int    status;
    bool   timed_out;
    bool   overflow;
} ChildResult;

/* Spawn argv, feed `input` on stdin, collect stdout (bounded) and the stderr
 * tail until both close or the deadline passes, then reap. Returns -1 only if
 * the child could not be started. */
static int run_child(char *const argv[], const char *input, int timeout_sec, ChildResult *res) {
    memset(res, 0, sizeof(*res));
    res->status = -1;

    pid_t pid = 0;
    int fd_in = -1, fd_out = -1, fd_err = -1;
    if (spawn_child(argv, &pid, &fd_in, &fd_out, &fd_err) != 0) return -1;

    size_t in_len = input ? strlen(input) : 0, in_off = 0;
    if (in_len == 0) { close(fd_in); fd_in = -1; }

    long deadline = now_ms() + (long)timeout_sec * 1000;
    while (fd_out >= 0 || fd_err >= 0) {
        long remaining = deadline - now_ms();
        if (remaining <= 0) { res->timed_out = true; break; }

        struct pollfd pfd[3];
        int n = 0, i_in = -1, i_out = -1, i_err = -1;
        if (fd_in >= 0)  { i_in = n;  pfd[n].fd = fd_in;  pfd[n].events = POLLOUT; pfd[n++].revents = 0; }
        if (fd_out >= 0) { i_out = n; pfd[n].fd = fd_out; pfd[n].events = POLLIN;  pfd[n++].revents = 0; }
        if (fd_err >= 0) { i_err = n; pfd[n].fd = fd_err; pfd[n].events = POLLIN;  pfd[n++].revents = 0; }

        int pr = poll(pfd, (nfds_t)n, remaining < CODEX_POLL_SLICE_MS ? (int)remaining : CODEX_POLL_SLICE_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        /* stdin: write what fits, close once done (codex reads until EOF). An
         * EPIPE means codex stopped reading; its exit status will say why. */
        if (i_in >= 0 && pfd[i_in].revents) {
            ssize_t w = write(fd_in, input + in_off, in_len - in_off);
            if (w > 0) in_off += (size_t)w;
            if (in_off >= in_len || (w < 0 && errno != EAGAIN && errno != EINTR) ||
                (pfd[i_in].revents & (POLLERR | POLLHUP))) {
                close(fd_in);
                fd_in = -1;
            }
        }

        if (i_out >= 0 && pfd[i_out].revents) {
            char tmp[8192];
            for (int k = 0; k < CODEX_DRAIN_READS; k++) {
                ssize_t r = read(fd_out, tmp, sizeof(tmp));
                if (r > 0) {
                    if (buf_append(&res->out, &res->out_len, &res->out_cap, CODEX_OUT_MAX, tmp, (size_t)r) != 0) {
                        res->overflow = true;
                        break;
                    }
                    continue;
                }
                if (r < 0 && errno == EINTR) continue;
                if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd_out); fd_out = -1; }
                break;
            }
            if (res->overflow) break;
        }

        if (i_err >= 0 && pfd[i_err].revents) {
            char tmp[4096];
            for (int k = 0; k < CODEX_DRAIN_READS; k++) {
                ssize_t r = read(fd_err, tmp, sizeof(tmp));
                if (r > 0) { tail_append(res->err, &res->err_len, tmp, (size_t)r); continue; }
                if (r < 0 && errno == EINTR) continue;
                if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd_err); fd_err = -1; }
                break;
            }
        }
    }

    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0) close(fd_out);
    if (fd_err >= 0) close(fd_err);

    /* Both pipes closed normally -> codex is exiting, but it still has to do so
     * before the request deadline. On timeout/overflow it is still running, so
     * terminate the group right away. */
    bool abort_child = res->timed_out || res->overflow;
    res->status = reap_child(pid, deadline, abort_child, &res->timed_out);
    return 0;
}

/* Last "ERROR: ..." line from codex stderr. codex echoes the whole prompt on
 * stderr, and the untrusted source text can itself contain a line starting with
 * "ERROR:", so a line that also occurs in the prompt is never picked - it would
 * be copied into the client response and the server log.
 * Returns a malloc'd string or NULL. */
static char *last_error_line(const char *err, const char *prompt) {
    const char *found = NULL;
    for (const char *p = err; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        if (strncmp(p, "ERROR:", 6) == 0) {
            char *line = strndup(p, line_len);
            if (line && (!prompt || !strstr(prompt, line))) found = p;
            free(line);
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!found) return NULL;
    found += 6;
    while (*found == ' ') found++;
    size_t len = strcspn(found, "\r\n");
    if (len > 300) len = 300;
    return strndup(found, len);
}

/* Read a whole small file (bounded). Returns malloc'd content, or NULL with
 * *failed=false when the file does not exist (codex wrote no message) and
 * *failed=true when it exists but is too large or could not be read - that
 * must fail the request rather than fall back to stdout. */
static char *read_file_bounded(const char *path, size_t max, bool *failed) {
    *failed = false;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        *failed = (errno != ENOENT);
        return NULL;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (buf_append(&buf, &len, &cap, max, tmp, n) != 0) {
            *failed = true;
            break;
        }
    }
    if (ferror(fp)) *failed = true;
    fclose(fp);
    if (*failed) {
        free(buf);
        return NULL;
    }
    return buf ? buf : strdup("");
}

/* ---- public API ----------------------------------------------------------- */

void codex_cli_probe(const char *bin, CodexCliInfo *info) {
    memset(info, 0, sizeof(*info));
    info->major = info->minor = info->patch = -1;
    if (!bin || !*bin) return;

    ChildResult res;
    char *const ver_argv[] = { (char *)bin, (char *)"--version", NULL };
    if (run_child(ver_argv, NULL, CODEX_PROBE_TIMEOUT, &res) == 0) {
        if (res.out && WIFEXITED(res.status) && WEXITSTATUS(res.status) == 0) {
            size_t l = strcspn(res.out, "\r\n");
            if (l >= sizeof(info->version)) l = sizeof(info->version) - 1;
            memcpy(info->version, res.out, l);
            info->version[l] = '\0';
            /* "codex-cli 0.166.0" -> 0 / 166 / 0 */
            const char *d = strpbrk(info->version, "0123456789");
            if (d && sscanf(d, "%d.%d.%d", &info->major, &info->minor, &info->patch) < 2) {
                info->major = info->minor = info->patch = -1;
            }
        }
        free(res.out);
    }

    char *const help_argv[] = { (char *)bin, (char *)"exec", (char *)"--help", NULL };
    if (run_child(help_argv, NULL, CODEX_PROBE_TIMEOUT, &res) == 0) {
        if (WIFEXITED(res.status) && WEXITSTATUS(res.status) == 0) {
            info->has_exec = true;
            info->has_ephemeral = res.out && strstr(res.out, "--ephemeral") != NULL;
            info->has_ignore_user_config = res.out && strstr(res.out, "--ignore-user-config") != NULL;
        }
        free(res.out);
    }

    /* MCP servers from the user's config.toml are started by every run, and
     * their tools sit outside the read-only sandbox. Count them for a warning. */
    info->mcp_enabled = -1;
    char *const mcp_argv[] = { (char *)bin, (char *)"mcp", (char *)"list", (char *)"--json", NULL };
    if (run_child(mcp_argv, NULL, CODEX_PROBE_TIMEOUT, &res) == 0) {
        cJSON *list = (res.out && WIFEXITED(res.status) && WEXITSTATUS(res.status) == 0)
                          ? cJSON_Parse(res.out) : NULL;
        if (cJSON_IsArray(list)) {
            info->mcp_enabled = 0;
            size_t used = 0;
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, list) {
                cJSON *name = cJSON_GetObjectItem(it, "name");
                if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "enabled")) || !cJSON_IsString(name)) continue;
                info->mcp_enabled++;
                int w = snprintf(info->mcp_names + used, sizeof(info->mcp_names) - used,
                                 "%s%s", used ? ", " : "", name->valuestring);
                if (w > 0) used += ((size_t)w < sizeof(info->mcp_names) - used)
                                       ? (size_t)w : sizeof(info->mcp_names) - used - 1;
            }
        }
        cJSON_Delete(list);
        free(res.out);
    }
}

bool codex_cli_has_mcp_server(const CodexCliInfo *info) {
    if (!info || info->major < 0) return true;  /* unknown: don't claim it is gone */
    if (info->major != CODEX_MCP_REMOVED_MAJOR) return info->major < CODEX_MCP_REMOVED_MAJOR;
    return info->minor < CODEX_MCP_REMOVED_MINOR;
}

char *codex_exec_run(const CodexExecOptions *opt, const char *prompt,
                     char **err_out, bool *timed_out, char **stderr_out) {
    if (err_out) *err_out = NULL;
    if (timed_out) *timed_out = false;
    if (stderr_out) *stderr_out = NULL;
    if (!opt || !opt->bin || !prompt || !*prompt) {
        if (err_out) *err_out = strdup("invalid arguments");
        return NULL;
    }

    /* -c values are parsed as TOML; config_loader restricts them to
     * [A-Za-z0-9._-]{1,CODEX_TOKEN_MAX} so plain double quotes are enough and
     * the buffers below cannot truncate (which would drop the closing quote). */
    if ((opt->approval_policy && strlen(opt->approval_policy) > CODEX_TOKEN_MAX) ||
        (opt->reasoning_effort && strlen(opt->reasoning_effort) > CODEX_TOKEN_MAX)) {
        if (err_out) *err_out = strdup("codex -c override value too long");
        return NULL;
    }
    char approval_kv[CODEX_TOKEN_MAX + 32] = "", effort_kv[CODEX_TOKEN_MAX + 32] = "";
    char out_path[PATH_MAX] = "";
    const char *argv[32];
    int a = 0;
    argv[a++] = opt->bin;
    argv[a++] = "exec";
    argv[a++] = "--skip-git-repo-check";   /* the per-request cwd is not a git repo */
    argv[a++] = "--color";
    argv[a++] = "never";
    if (opt->ephemeral) argv[a++] = "--ephemeral";
    if (opt->ignore_user_config) argv[a++] = "--ignore-user-config";
    if (opt->sandbox && *opt->sandbox) {
        argv[a++] = "-s";
        argv[a++] = opt->sandbox;
    }
    if (opt->approval_policy && *opt->approval_policy) {
        snprintf(approval_kv, sizeof(approval_kv), "approval_policy=\"%s\"", opt->approval_policy);
        argv[a++] = "-c";
        argv[a++] = approval_kv;
    }
    if (opt->reasoning_effort && *opt->reasoning_effort) {
        snprintf(effort_kv, sizeof(effort_kv), "model_reasoning_effort=\"%s\"", opt->reasoning_effort);
        argv[a++] = "-c";
        argv[a++] = effort_kv;
    }
    if (opt->model && *opt->model) {
        argv[a++] = "-m";
        argv[a++] = opt->model;
    }
    if (opt->cwd && *opt->cwd) {
        snprintf(out_path, sizeof(out_path), "%s/%s", opt->cwd, CODEX_LAST_MSG_FILE);
        argv[a++] = "-C";
        argv[a++] = opt->cwd;
        argv[a++] = "-o";
        argv[a++] = out_path;
    }
    argv[a++] = "-";                       /* prompt comes from stdin */
    argv[a] = NULL;

    ChildResult res;
    if (run_child((char *const *)argv, prompt, opt->timeout_sec, &res) != 0) {
        if (err_out) *err_out = strdup("failed to start codex exec");
        return NULL;
    }

    char *text = NULL;
    bool file_failed = false;
    if (out_path[0]) {
        text = read_file_bounded(out_path, CODEX_OUT_MAX, &file_failed);
        unlink(out_path);                  /* lets the caller rmdir the cwd */
    }
    if (file_failed) res.overflow = true;  /* too large or unreadable: fail, no stdout fallback */

    bool exited_ok = WIFEXITED(res.status) && WEXITSTATUS(res.status) == 0;
    if (stderr_out && res.err_len) *stderr_out = strdup(res.err);

    if (res.timed_out || res.overflow || !exited_ok) {
        free(text);
        text = NULL;
        if (timed_out) *timed_out = res.timed_out;
        if (err_out) {
            if (res.timed_out) {
                *err_out = strdup("codex exec timed out");
            } else if (res.overflow) {
                *err_out = strdup(file_failed ? "codex response too large or unreadable"
                                              : "codex response too large");
            } else {
                char *line = last_error_line(res.err, prompt);
                char buf[400];
                if (WIFEXITED(res.status)) {
                    snprintf(buf, sizeof(buf), "codex exec failed (exit %d)%s%s",
                             WEXITSTATUS(res.status), line ? ": " : "", line ? line : "");
                } else {
                    snprintf(buf, sizeof(buf), "codex exec terminated abnormally%s%s",
                             line ? ": " : "", line ? line : "");
                }
                free(line);
                *err_out = strdup(buf);
            }
        }
        free(res.out);
        return NULL;
    }

    /* Prefer --output-last-message; stdout carries the same final message in
     * non-TTY mode and covers runs without a cwd. */
    if (!text || !*text) {
        free(text);
        text = res.out;
        res.out = NULL;
    }
    free(res.out);

    if (!text || !*text) {
        free(text);
        if (err_out) *err_out = strdup("codex exec returned no message");
        return NULL;
    }
    return text;
}
