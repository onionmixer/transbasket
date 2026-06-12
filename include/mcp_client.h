#ifndef MCP_CLIENT_H
#define MCP_CLIENT_H

#include <sys/types.h>   /* pid_t */
#include <stddef.h>      /* size_t */
#include <stdbool.h>     /* bool */
#include <cjson/cJSON.h> /* cJSON (mcp_call_tool arguments) */

/*
 * Minimal MCP (Model Context Protocol) stdio JSON-RPC 2.0 client.
 *
 * Spawns `codex mcp-server` as a child process, performs the initialize
 * handshake, and calls the `codex` tool to obtain a translation. One client
 * instance is owned by a single thread (spawn-per-request); it is never shared.
 */

typedef struct {
    pid_t  pid;            /* child pid (0 if not running) */
    int    fd_in;          /* write to child stdin */
    int    fd_out;         /* read from child stdout */
    int    fd_err;         /* read from child stderr (failure diagnostics) */

    char  *rbuf;           /* stdout line-assembly buffer (append-only) */
    size_t rbuf_len;       /* current bytes in rbuf */
    size_t rbuf_cap;       /* allocated capacity of rbuf */

    char  *errbuf;         /* captured stderr (bounded) */
    size_t errbuf_len;

    int    next_id;        /* next JSON-RPC request id */

    /* diagnostics / observability */
    int    last_exit_status;  /* waitpid status, -1 if not reaped */
    size_t bytes_read;
    size_t bytes_written;
} MCPClient;

/* Spawn `<bin> mcp-server`. Returns NULL on failure. */
MCPClient *mcp_client_start(const char *bin);

/* Perform initialize + notifications/initialized handshake.
 * Returns 0 on success, -1 on failure/timeout. */
int mcp_initialize(MCPClient *c, int timeout_sec);

/* Generic MCP tools/call. Builds the JSON-RPC envelope for `tool_name` with the
 * given `arguments` object and returns the tool's text result (caller frees),
 * or NULL on failure.
 *
 * OWNERSHIP: this function TAKES OWNERSHIP of `arguments` and frees it (even on
 * error / NULL input). The caller must NOT delete `arguments` afterwards. Pass
 * NULL for an empty argument object.
 *
 * On failure, *err_out (caller frees) gets a message and *timed_out is set.
 * Reusable by any MCP-backed translation backend. */
char *mcp_call_tool(MCPClient *c,
                    const char *tool_name,
                    cJSON *arguments,
                    int timeout_sec,
                    char **err_out,
                    bool *timed_out);

/* Call tools/call name="codex" (thin wrapper over mcp_call_tool). Returns the
 * agent's final message text (caller frees), or NULL on failure.
 *   - reasoning_effort: if non-NULL/non-empty, injected as
 *     arguments.config.model_reasoning_effort
 *   - on failure, *err_out (caller frees) gets a message and *timed_out is set.
 */
char *mcp_call_codex(MCPClient *c,
                     const char *prompt,
                     const char *model,
                     const char *sandbox,
                     const char *approval_policy,
                     const char *cwd,
                     const char *reasoning_effort,
                     int timeout_sec,
                     char **err_out,
                     bool *timed_out);

/* Close pipes, terminate and reap the child, free the client. */
void mcp_client_stop(MCPClient *c);

#endif /* MCP_CLIENT_H */
