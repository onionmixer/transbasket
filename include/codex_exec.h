#ifndef CODEX_EXEC_H
#define CODEX_EXEC_H

#include <stddef.h>
#include <stdbool.h>

/*
 * `codex exec` driver - the codex backend's default interface.
 *
 * codex-cli 0.154.0 removed the `codex mcp-server` subcommand (absent from the
 * 0.166.0 binary as well), so the MCP route in mcp_client.c only works with older
 * CLIs. `codex exec` is the supported non-interactive surface: the prompt is fed
 * on stdin (never argv, so untrusted text does not show up in ps or
 * /proc/<pid>/cmdline) and the final agent message is taken from
 * --output-last-message, falling back to stdout.
 *
 * One call = one child process, owned by the calling thread.
 */

typedef struct {
    const char *bin;               /* codex executable (PATH-searched) */
    const char *model;             /* -m, NULL = codex default */
    const char *sandbox;           /* -s */
    const char *approval_policy;   /* -c approval_policy="..." */
    const char *reasoning_effort;  /* -c model_reasoning_effort="...", NULL = not set */
    const char *cwd;               /* -C and home of the output file, NULL = inherit */
    bool        ephemeral;         /* pass --ephemeral (no session files on disk) */
    bool        ignore_user_config;/* pass --ignore-user-config (no MCP servers, plugins, ...) */
    int         timeout_sec;
} CodexExecOptions;

/* What the installed CLI offers, detected once at startup. */
typedef struct {
    char version[64];              /* first line of `<bin> --version`, "" if unknown */
    int  major, minor, patch;      /* parsed from version, -1 if unparsable */
    bool has_exec;                 /* `<bin> exec --help` exits 0 */
    bool has_ephemeral;            /* exec help advertises --ephemeral */
    bool has_ignore_user_config;   /* exec help advertises --ignore-user-config */
    int  mcp_enabled;              /* enabled servers in `<bin> mcp list --json`, -1 if unknown */
    char mcp_names[256];           /* their names, comma separated (truncated) */
} CodexCliInfo;

/* Probe the installed CLI. Never fails hard; unknown fields stay false/""/-1. */
void codex_cli_probe(const char *bin, CodexCliInfo *info);

/* false when the probed version is known to be >= 0.154.0, which no longer has
 * `codex mcp-server`. An unknown version returns true (no false alarm). */
bool codex_cli_has_mcp_server(const CodexCliInfo *info);

/* Run one `codex exec` turn and return the final message (caller frees), or
 * NULL on failure with *err_out (caller frees) set to a short message that is
 * safe to show to clients - codex echoes the prompt on stderr, so raw stderr is
 * never put there. If stderr_out is non-NULL it receives the bounded stderr
 * tail for debug traces (caller frees). */
char *codex_exec_run(const CodexExecOptions *opt, const char *prompt,
                     char **err_out, bool *timed_out, char **stderr_out);

#endif /* CODEX_EXEC_H */
