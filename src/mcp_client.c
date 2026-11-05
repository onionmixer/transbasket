/*
 * Minimal MCP stdio JSON-RPC 2.0 client for driving `codex mcp-server`.
 * See include/mcp_client.h for the public contract.
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
#include <sys/wait.h>
#include <cjson/cJSON.h>

#include "mcp_client.h"
#include "utils.h"

extern char **environ;

#define MCP_RBUF_INIT     8192
#define MCP_RBUF_MAX      (4 * 1024 * 1024)   /* hard cap on stdout assembly */
#define MCP_ERRBUF_MAX    4096
#define MCP_POLL_SLICE_MS 100

/* ---- small helpers -------------------------------------------------------- */

/* Monotonic milliseconds for deadline math (immune to wall-clock changes). */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Write the whole buffer, retrying EINTR/EAGAIN. Returns 0 on success. */
static int write_all(MCPClient *c, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (!translation_remaining_ms(c->control) || now_ms() >= c->io_deadline_ms) return -1;
        ssize_t n = write(c->fd_in, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Nonblocking stdin: never let a child that stops reading pin us. */
                struct pollfd p = { .fd = c->fd_in, .events = POLLOUT };
                poll(&p, 1, 100);
                continue;
            }
            return -1;  /* EPIPE: child gone */
        }
        off += (size_t)n;
        c->bytes_written += (size_t)n;
    }
    return 0;
}

/* Serialize a JSON-RPC object, append '\n', send. Does NOT free `obj`. */
static int send_json(MCPClient *c, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return -1;
    size_t len = strlen(s);
    char *line = malloc(len + 2);
    if (!line) { free(s); return -1; }
    memcpy(line, s, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    free(s);
    int rc = write_all(c, line, len + 1);
    free(line);
    return rc;
}

/* Append `data` to rbuf, growing as needed. Returns 0 on success. */
static int rbuf_append(MCPClient *c, const char *data, size_t len) {
    if (c->rbuf_len + len + 1 > c->rbuf_cap) {
        size_t newcap = c->rbuf_cap ? c->rbuf_cap : MCP_RBUF_INIT;
        while (newcap < c->rbuf_len + len + 1) {
            if (newcap >= MCP_RBUF_MAX) return -1;  /* runaway */
            newcap *= 2;
        }
        char *p = realloc(c->rbuf, newcap);
        if (!p) return -1;
        c->rbuf = p;
        c->rbuf_cap = newcap;
    }
    memcpy(c->rbuf + c->rbuf_len, data, len);
    c->rbuf_len += len;
    c->rbuf[c->rbuf_len] = '\0';
    return 0;
}

/* Drain fd_err into errbuf (bounded); keep reading past the cap to avoid the
 * child blocking on a full stderr pipe. */
static void drain_stderr(MCPClient *c) {
    char tmp[4096];
    for (int i = 0; i < 16; i++) {
        ssize_t n = read(c->fd_err, tmp, sizeof(tmp));
        if (n > 0) {
            size_t room = (c->errbuf_len < MCP_ERRBUF_MAX)
                              ? (MCP_ERRBUF_MAX - c->errbuf_len) : 0;
            if (room > 0) {
                size_t copy = ((size_t)n < room) ? (size_t)n : room;
                memcpy(c->errbuf + c->errbuf_len, tmp, copy);
                c->errbuf_len += copy;
                c->errbuf[c->errbuf_len] = '\0';
            }
            /* else: discard but keep draining */
            continue;
        }
        break;  /* EAGAIN / EOF / error */
    }
}

/*
 * Wait for a JSON-RPC response whose "id" == want_id. Non-matching messages
 * (notifications such as codex/event, other ids) are discarded. Drains stderr
 * each loop to prevent deadlock. Returns the matched cJSON message (caller
 * deletes), or NULL on timeout / EOF / error.
 */
static cJSON *wait_for_id(MCPClient *c, int want_id, int timeout_sec,
                          bool *timed_out, char **err_out) {
    if (timed_out) *timed_out = false;
    (void)timeout_sec;
    long deadline = c->io_deadline_ms;

    for (;;) {
        long remaining = deadline - now_ms();
        if (remaining <= 0 || !translation_remaining_ms(c->control)) {
            if (timed_out) *timed_out = true;
            if (err_out) *err_out = strdup("codex MCP call timed out");
            return NULL;
        }

        int slice = (remaining < MCP_POLL_SLICE_MS) ? (int)remaining : MCP_POLL_SLICE_MS;
        struct pollfd pfd[2];
        pfd[0].fd = c->fd_out; pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = c->fd_err; pfd[1].events = POLLIN; pfd[1].revents = 0;

        int pr = poll(pfd, 2, slice);
        if (pr < 0) {
            if (errno == EINTR) continue;  /* recompute deadline, retry */
            if (err_out) *err_out = strdup(strerror(errno));
            return NULL;
        }
        if (pr == 0) continue;  /* slice timeout; loop checks deadline */

        if (pfd[1].revents & POLLIN) {
            drain_stderr(c);
        }

        if (pfd[0].revents & POLLIN) {
            char tmp[8192];
            bool got_eof = false;
            for (int reads = 0; reads < 16; reads++) {
                ssize_t n = read(c->fd_out, tmp, sizeof(tmp));
                if (n > 0) {
                    c->bytes_read += (size_t)n;
                    if (rbuf_append(c, tmp, (size_t)n) != 0) {
                        if (err_out) *err_out = strdup("codex MCP response too large");
                        return NULL;
                    }
                    continue;
                }
                if (n == 0) { got_eof = true; break; }
                if (errno == EINTR) continue;
                break;  /* EAGAIN: no more for now */
            }

            /* Process every complete '\n'-terminated line. */
            char *start = c->rbuf;
            char *nl;
            while ((nl = memchr(start, '\n', c->rbuf_len - (size_t)(start - c->rbuf))) != NULL) {
                *nl = '\0';
                if (*start != '\0') {
                    cJSON *msg = cJSON_Parse(start);
                    if (msg) {
                        cJSON *id = cJSON_GetObjectItem(msg, "id");
                        if (cJSON_IsNumber(id) && id->valueint == want_id) {
                            /* matched: consume this line and keep the rest for
                             * the next wait. The '\n' is already '\0', so a
                             * stale line left in rbuf would swallow the next
                             * message - a server that answers tools/call with
                             * no notifications in between would never match. */
                            char *next = nl + 1;
                            size_t tail = c->rbuf_len - (size_t)(next - c->rbuf);
                            memmove(c->rbuf, next, tail);
                            c->rbuf_len = tail;
                            c->rbuf[c->rbuf_len] = '\0';
                            return msg;
                        }
                        cJSON_Delete(msg);  /* notification / other id */
                    }
                }
                start = nl + 1;
            }
            /* Keep the unterminated tail. */
            size_t consumed = (size_t)(start - c->rbuf);
            if (consumed > 0) {
                size_t tail = c->rbuf_len - consumed;
                memmove(c->rbuf, start, tail);
                c->rbuf_len = tail;
                c->rbuf[c->rbuf_len] = '\0';
            }

            if (got_eof || (pfd[0].revents & (POLLHUP | POLLERR))) {
                /* child closed stdout before answering */
                drain_stderr(c);
                if (err_out) {
                    char buf[MCP_ERRBUF_MAX + 64];
                    snprintf(buf, sizeof(buf), "codex exited before responding%s%s",
                             c->errbuf_len ? ": " : "",
                             c->errbuf_len ? c->errbuf : "");
                    *err_out = strdup(buf);
                }
                return NULL;
            }
        } else if (pfd[0].revents & (POLLHUP | POLLERR)) {
            drain_stderr(c);
            if (err_out) {
                char buf[MCP_ERRBUF_MAX + 64];
                snprintf(buf, sizeof(buf), "codex stdout closed%s%s",
                         c->errbuf_len ? ": " : "",
                         c->errbuf_len ? c->errbuf : "");
                *err_out = strdup(buf);
            }
            return NULL;
        }
    }
}

/* Extract the translation text from a tools/call result object. */
static char *extract_translation(cJSON *result) {
    if (!result) return NULL;

    /* Prefer structuredContent.content (already JSON-decoded single string). */
    cJSON *sc = cJSON_GetObjectItem(result, "structuredContent");
    if (sc) {
        cJSON *content = cJSON_GetObjectItem(sc, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            return strdup(content->valuestring);
        }
    }

    /* Fallback: concatenate result.content[].text. */
    cJSON *carr = cJSON_GetObjectItem(result, "content");
    if (cJSON_IsArray(carr)) {
        size_t total = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, carr) {
            cJSON *t = cJSON_GetObjectItem(it, "text");
            if (cJSON_IsString(t) && t->valuestring) total += strlen(t->valuestring);
        }
        char *out = calloc(total + 1, 1);
        if (!out) return NULL;
        cJSON_ArrayForEach(it, carr) {
            cJSON *t = cJSON_GetObjectItem(it, "text");
            if (cJSON_IsString(t) && t->valuestring) strcat(out, t->valuestring);
        }
        return out;
    }
    return NULL;
}

/* ---- public API ----------------------------------------------------------- */

MCPClient *mcp_client_start(const char *bin) {
    if (!bin || !*bin) return NULL;

    MCPClient *c = calloc(1, sizeof(MCPClient));
    if (!c) return NULL;
    c->next_id = 1;
    c->last_exit_status = -1;
    c->errbuf = calloc(MCP_ERRBUF_MAX + 1, 1);
    if (!c->errbuf) { free(c); return NULL; }

    int in_pipe[2]  = { -1, -1 };  /* parent writes in_pipe[1] -> child stdin in_pipe[0] */
    int out_pipe[2] = { -1, -1 };  /* child stdout out_pipe[1] -> parent reads out_pipe[0] */
    int err_pipe[2] = { -1, -1 };

    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        LOG_INFO("Error: mcp_client_start pipe() failed: %s", strerror(errno));
        goto fail;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);

    /* Wire child stdio, then close every other inherited fd (>=3) so the MHD
     * listen socket, cache DB, logs, etc. never leak into codex. */
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);

    /* Reset signal mask/dispositions so codex does not inherit a blocked mask
     * or our ignored SIGPIPE. Put the child in its own process group so we can
     * kill the WHOLE tree later: `codex` is a node wrapper that spawns the real
     * codex binary as a grandchild; killing only the direct child would orphan
     * the grandchild. */
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    sigset_t all;
    sigfillset(&all);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);  /* child becomes leader of a new group */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                    POSIX_SPAWN_SETPGROUP);

    char *const argv[] = { (char *)bin, (char *)"mcp-server", NULL };
    int rc = posix_spawnp(&c->pid, bin, &fa, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        LOG_INFO("Error: posix_spawnp(%s mcp-server) failed: %s", bin, strerror(rc));
        c->pid = 0;
        goto fail;
    }

    /* Parent keeps its ends; close the child ends. */
    close(in_pipe[0]);  in_pipe[0]  = -1;
    close(out_pipe[1]); out_pipe[1] = -1;
    close(err_pipe[1]); err_pipe[1] = -1;

    c->fd_in  = in_pipe[1];
    c->fd_out = out_pipe[0];
    c->fd_err = err_pipe[0];

    set_cloexec(c->fd_in);
    set_cloexec(c->fd_out);
    set_cloexec(c->fd_err);
    set_nonblock(c->fd_in);
    set_nonblock(c->fd_out);
    set_nonblock(c->fd_err);

    return c;

fail:
    if (in_pipe[0]  >= 0) close(in_pipe[0]);
    if (in_pipe[1]  >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    free(c->errbuf);
    free(c);
    return NULL;
}

int mcp_initialize(MCPClient *c, int timeout_sec) {
    if (!c) return -1;
    c->io_deadline_ms = now_ms() + (long)timeout_sec * 1000;

    int id = c->next_id++;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "initialize");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", "2025-06-18");
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddStringToObject(ci, "name", "transbasket");
    cJSON_AddStringToObject(ci, "version", "1.0.0");
    cJSON_AddItemToObject(params, "clientInfo", ci);
    cJSON_AddItemToObject(req, "params", params);

    int rc = send_json(c, req);
    cJSON_Delete(req);
    if (rc != 0) {
        LOG_INFO("Error: failed to send MCP initialize");
        return -1;
    }

    bool timed_out = false;
    char *err = NULL;
    cJSON *resp = wait_for_id(c, id, timeout_sec, &timed_out, &err);
    if (!resp) {
        LOG_INFO("Error: MCP initialize failed: %s", err ? err : "unknown");
        free(err);
        return -1;
    }
    cJSON_Delete(resp);
    free(err);

    /* notifications/initialized (no id) */
    cJSON *note = cJSON_CreateObject();
    cJSON_AddStringToObject(note, "jsonrpc", "2.0");
    cJSON_AddStringToObject(note, "method", "notifications/initialized");
    rc = send_json(c, note);
    cJSON_Delete(note);
    return rc;
}

/* Generic tools/call. Takes ownership of `arguments` (frees it on every path). */
char *mcp_call_tool(MCPClient *c,
                    const char *tool_name,
                    cJSON *arguments,
                    int timeout_sec,
                    char **err_out,
                    bool *timed_out) {
    if (err_out) *err_out = NULL;
    if (timed_out) *timed_out = false;
    if (!c || !tool_name) {
        if (arguments) cJSON_Delete(arguments);  /* honor ownership contract */
        if (err_out) *err_out = strdup("invalid arguments");
        return NULL;
    }
    c->io_deadline_ms = now_ms() + (long)timeout_sec * 1000;
    if (!arguments) arguments = cJSON_CreateObject();

    int id = c->next_id++;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "tools/call");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", tool_name);
    cJSON_AddItemToObject(params, "arguments", arguments);  /* transfer ownership */
    cJSON_AddItemToObject(req, "params", params);

    int rc = send_json(c, req);
    cJSON_Delete(req);  /* frees req -> params -> arguments (single owner) */
    if (rc != 0) {
        if (err_out) *err_out = strdup("failed to send tools/call");
        return NULL;
    }

    char *werr = NULL;
    cJSON *resp = wait_for_id(c, id, timeout_sec, timed_out, &werr);
    if (!resp) {
        if (err_out) *err_out = werr ? werr : strdup("MCP tool call failed");
        else free(werr);
        return NULL;
    }
    free(werr);

    /* top-level error? */
    cJSON *jerr = cJSON_GetObjectItem(resp, "error");
    if (jerr) {
        cJSON *msg = cJSON_GetObjectItem(jerr, "message");
        if (err_out) {
            *err_out = strdup((cJSON_IsString(msg) && msg->valuestring)
                                  ? msg->valuestring : "MCP tool returned an error");
        }
        cJSON_Delete(resp);
        return NULL;
    }

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *is_error = result ? cJSON_GetObjectItem(result, "isError") : NULL;
    if (cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)) {
        char *txt = extract_translation(result);
        if (err_out) {
            char buf[512];
            snprintf(buf, sizeof(buf), "MCP tool error%s%s",
                     txt ? ": " : "", txt ? txt : "");
            *err_out = strdup(buf);
        }
        free(txt);
        cJSON_Delete(resp);
        return NULL;
    }

    char *text = extract_translation(result);
    cJSON_Delete(resp);

    if (!text) {
        if (err_out) *err_out = strdup("no content in MCP tool response");
        return NULL;
    }
    return text;
}

/* codex-specific wrapper: builds the codex tool arguments, then delegates to
 * mcp_call_tool. The JSON sent is identical to the previous direct builder. */
char *mcp_call_codex(MCPClient *c,
                     const char *prompt,
                     const char *model,
                     const char *sandbox,
                     const char *approval_policy,
                     const char *cwd,
                     const char *reasoning_effort,
                     int timeout_sec,
                     char **err_out,
                     bool *timed_out) {
    if (err_out) *err_out = NULL;
    if (timed_out) *timed_out = false;
    if (!c || !prompt) {
        if (err_out) *err_out = strdup("invalid arguments");
        return NULL;
    }

    cJSON *args = cJSON_CreateObject();
    if (!args) {
        if (err_out) *err_out = strdup("out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(args, "prompt", prompt);
    if (model && *model)             cJSON_AddStringToObject(args, "model", model);
    if (sandbox && *sandbox)         cJSON_AddStringToObject(args, "sandbox", sandbox);
    if (approval_policy && *approval_policy)
        cJSON_AddStringToObject(args, "approval-policy", approval_policy);
    if (cwd && *cwd)                 cJSON_AddStringToObject(args, "cwd", cwd);
    if (reasoning_effort && *reasoning_effort) {
        cJSON *cfg = cJSON_CreateObject();
        cJSON_AddStringToObject(cfg, "model_reasoning_effort", reasoning_effort);
        cJSON_AddItemToObject(args, "config", cfg);
    }

    /* mcp_call_tool takes ownership of args. */
    return mcp_call_tool(c, "codex", args, timeout_sec, err_out, timed_out);
}

/* Terminate and reap the child, then free everything. */
void mcp_client_stop(MCPClient *c) {
    if (!c) return;

    if (c->fd_in  >= 0) close(c->fd_in);
    if (c->fd_out >= 0) close(c->fd_out);
    if (c->fd_err >= 0) close(c->fd_err);

    if (c->pid > 0) {
        /* Closing stdin signals EOF; give codex a brief chance, then SIGTERM,
         * then SIGKILL. Signal the whole PROCESS GROUP (negative pid) so the
         * grandchild codex binary spawned by the `codex` node wrapper is also
         * terminated and not orphaned. waitpid loops tolerate EINTR. */
        kill(-c->pid, SIGTERM);
        long grace = now_ms() + 2000;
        long left = translation_remaining_ms(c->control);
        if (grace > now_ms() + left) grace = now_ms() + left;
        for (;;) {
            siginfo_t info = {0};
            int rc = waitid(P_PID, (id_t)c->pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if ((rc == 0 && info.si_pid == c->pid) || (rc < 0 && errno != EINTR) || now_ms() >= grace) break;
            struct timespec delay = {0, 20 * 1000000};
            nanosleep(&delay, NULL);
        }
        kill(-c->pid, SIGKILL);
        int status = -1;
        while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }
        c->last_exit_status = status;
    }

    free(c->rbuf);
    free(c->errbuf);
    free(c);
}
