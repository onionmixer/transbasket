#ifndef DAEMON_H
#define DAEMON_H

/* -d daemon mode with a start report (PLAN_OPEN_ISSUES.md A).
 *
 * daemon_begin() forks twice. The process that ran transbasket stays in the
 * foreground: it copies the daemon's startup output to its own stderr and exits 0
 * once the daemon reports that the server is listening, or 1 if the daemon exits
 * first or does not report within DAEMON_READY_TIMEOUT seconds. Only the daemon
 * returns from daemon_begin() (0); -1 means forking was not possible and the caller
 * is still the original process.
 *
 * The daemon keeps the working directory and umask it was started with, so
 * relative paths mean what they mean in the foreground. Until daemon_ready() its
 * stdout and stderr are the pipe to the launcher (and, with a log file, every log
 * line is also appended there); daemon_ready() moves them to the log file or
 * /dev/null and then reports. */

#ifndef DAEMON_READY_TIMEOUT          /* seconds; tests build with a shorter one */
#define DAEMON_READY_TIMEOUT 60
#endif

int daemon_begin(const char *log_path);
/* 0 = reported, 1 = *stop was set (nothing reported), -1 = error. */
#include <signal.h>
int daemon_ready(volatile sig_atomic_t *stop);

#endif /* DAEMON_H */
