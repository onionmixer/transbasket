/**
 * -d daemon mode with a start report to the launcher (PLAN_OPEN_ISSUES.md A).
 */

#define _GNU_SOURCE   /* pipe2() */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "daemon.h"
#include "utils.h"

static int g_status_w = -1;   /* daemon side of the status pipe */
static int g_log_fd = -1;     /* -l file, opened before the report */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Write all of buf to fd, waiting for it to be writable at most until deadline.
 * false when the deadline passed or the fd failed: the launcher then stops copying. */
static bool write_until(int fd, const char *buf, size_t len, long long deadline) {
    while (len > 0) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            return false;
        }
        struct pollfd p = {fd, POLLOUT, 0};
        int r = poll(&p, 1, left > 1000 ? 1000 : (int)left);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 || (r > 0 && (p.revents & (POLLERR | POLLNVAL)))) {
            return false;
        }
        if (r == 0) {
            continue;
        }
        size_t chunk = len > 512 ? 512 : len;   /* small writes: a ready fd takes them */
        ssize_t n = write(fd, buf, chunk);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/* The launcher: copy the log pipe to stderr, read the status pipe, exit. */
static void launcher(pid_t child, int st_r, int lg_r) {
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        /* the intermediate child exits right after the second fork */
    }
    long long deadline = now_ms() + (long long)DAEMON_READY_TIMEOUT * 1000;
    bool log_open = true, st_open = true, copying = true, timed_out = false;
    char status[64];
    size_t slen = 0;
    char buf[4096];

    while (log_open || st_open) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        struct pollfd fds[2] = {{log_open ? lg_r : -1, POLLIN, 0}, {st_open ? st_r : -1, POLLIN, 0}};
        int r = poll(fds, 2, left > 1000 ? 1000 : (int)left);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (log_open && fds[0].revents) {
            ssize_t n = read(lg_r, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                log_open = false;
            } else if (copying) {
                copying = write_until(STDERR_FILENO, buf, (size_t)n, deadline);
            }
        }
        if (st_open && fds[1].revents) {
            ssize_t n = read(st_r, status + slen, sizeof(status) - 1 - slen);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                st_open = false;
            } else {
                slen += (size_t)n;
            }
        }
    }
    if (timed_out && st_open) {
        /* the report may already be waiting (e.g. copying the log used up the time) */
        struct pollfd p = {st_r, POLLIN, 0};
        if (poll(&p, 1, 0) > 0) {
            ssize_t n = read(st_r, status + slen, sizeof(status) - 1 - slen);
            if (n > 0) {
                slen += (size_t)n;
            }
        }
    }
    status[slen] = '\0';

    char msg[256];
    int rc;
    if (slen > 0 && status[0] == 'R') {
        snprintf(msg, sizeof(msg), "transbasket is running in the background (pid %ld)\n", strtol(status + 1, NULL, 10));
        rc = 0;
    } else if (timed_out) {
        snprintf(msg, sizeof(msg), "Error: the daemon did not report within %d s; it may still be starting - check its log\n",
                 DAEMON_READY_TIMEOUT);
        rc = 1;
    } else {
        snprintf(msg, sizeof(msg), "Error: the daemon failed to start (see the messages above)\n");
        rc = 1;
    }
    write_until(STDERR_FILENO, msg, strlen(msg), now_ms() + 2000);
    _exit(rc);
}

int daemon_begin(const char *log_path) {
    /* A launcher that went away must not kill the daemon through a write. */
    signal(SIGPIPE, SIG_IGN);

    /* 0, 1 and 2 must be taken before the pipes are made, or a pipe could become
     * "stdout" by accident when the daemon was started with them closed. */
    int fd;
    while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd <= 2) {
    }
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open /dev/null: %s\n", strerror(errno));
        return -1;
    }
    close(fd);

    int st[2], lg[2];
    if (pipe2(st, O_CLOEXEC) != 0) {
        fprintf(stderr, "Error: pipe2() failed: %s\n", strerror(errno));
        return -1;
    }
    if (pipe2(lg, O_CLOEXEC) != 0) {
        fprintf(stderr, "Error: pipe2() failed: %s\n", strerror(errno));
        close(st[0]);
        close(st[1]);
        return -1;
    }

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork() failed: %s\n", strerror(errno));
        close(st[0]); close(st[1]); close(lg[0]); close(lg[1]);
        return -1;
    }
    if (pid > 0) {
        close(st[1]);
        close(lg[1]);
        launcher(pid, st[0], lg[0]);   /* does not return */
    }

    close(st[0]);
    close(lg[0]);
    if (setsid() < 0) {
        _exit(1);
    }
    pid = fork();
    if (pid < 0) {
        _exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }

    /* The daemon. Startup output goes to the launcher from here on. */
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 || dup2(lg[1], STDOUT_FILENO) < 0 ||
        dup2(lg[1], STDERR_FILENO) < 0) {
        _exit(1);
    }
    if (devnull > 2) close(devnull);
    close(lg[1]);
    g_status_w = st[1];

    if (log_path) {
        g_log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (g_log_fd < 0) {
            LOG_INFO("Error: cannot open log file %s: %s", log_path, strerror(errno));
            exit(1);
        }
        log_set_tee(g_log_fd);   /* startup lines go to the launcher and to the file */
    }
    return 0;
}

int daemon_ready(void) {
    int fd = g_log_fd >= 0 ? g_log_fd : open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        return -1;
    }
    log_set_tee(-1);   /* stdout is the file itself now */
    if (fd > 2) {
        close(fd);
    }
    g_log_fd = -1;

    char msg[32];
    int n = snprintf(msg, sizeof(msg), "R%ld\n", (long)getpid());
    ssize_t w;
    do {
        w = write(g_status_w, msg, (size_t)n);   /* EPIPE if the launcher is gone: carry on */
    } while (w < 0 && errno == EINTR);
    close(g_status_w);
    g_status_w = -1;
    return 0;
}
