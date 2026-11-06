#ifndef CODEX_DRIVER_H
#define CODEX_DRIVER_H

/* How the codex backend runs the codex CLI (PLAN_BACKEND_MODULES.md step 2).
 *
 * src/backend_codex.c owns everything that does not depend on how codex is invoked:
 * the concurrency slot and its deadline, the per-request temp cwd, the schema file,
 * the prompt with its delimiter-isolated source, the NUL check, structured parsing,
 * post-processing, the status codes (failure 502, deadline 504) and the DEBUG trace.
 * A driver only starts codex and returns its final message. Today there is one
 * driver, `codex exec` (src/codex_driver_exec.c). Adding one: a driver file, a row in
 * CODEX_DRIVERS (backend_codex.c), storing the chosen CODEX_INTERFACE value in Config
 * (config_loader.c parses and validates it; today only "exec" is accepted) and the
 * "Codex Interface:" startup line in main.c. */

#include <stdbool.h>
#include <stddef.h>
#include "config_loader.h"
#include "translation_control.h"

typedef struct {
    bool structured_output;   /* can return output for a JSON schema file (SOURCE_HINT_MODE=model) */
} CodexDriverCaps;

/* What the backend decided for one translation. Pointers are valid for the call only. */
typedef struct {
    const char *prompt;
    const char *cwd;          /* per-request temp dir, NULL = none */
    const char *schema_path;  /* NULL = plain output; else ask codex for output in this schema
                               * (the driver does not check it - the backend validates) */
    const TranslationControl *control;
} CodexRunRequest;

typedef struct CodexDriver {
    const char *name;         /* the CODEX_INTERFACE value */
    /* Startup: probe the CLI, log the driver's own lines and warnings, fill *caps and
     * *state. A missing or old CLI only warns. -1 only when the driver's state cannot be
     * allocated (decision S1); nothing is left allocated then. */
    int  (*init)(const Config *config, void **state, CodexDriverCaps *caps);
    void (*free)(void *state);
    /* One translation, synchronous. Honours req->control (cancellation) and
     * CODEX_TIMEOUT, reaps every child it started and tries to remove its own files
     * from req->cwd before returning, so the caller can rmdir it (best effort: the
     * exec driver does not check its unlink). Called concurrently from several request
     * threads with the same state, which is read-only after init.
     * Returns a writable buffer the caller frees, NUL-terminated after *len_out bytes;
     * *len_out counts bytes up to that terminator, including any NUL inside the
     * output. NULL on failure, with *timed_out set on a deadline and *err_out (caller
     * frees) normally set to a short message that is shown to the client - it may stay
     * NULL when memory runs out. The exec driver builds that message from codex's last
     * stderr `ERROR:` line and skips such a line only when it occurs verbatim in the
     * prompt (codex echoes the prompt on stderr); it is not a general guarantee that no
     * prompt text gets through. *stderr_out, only when stderr_out is not NULL (DEBUG),
     * is a bounded stderr tail the caller frees. */
    char *(*run)(void *state, const Config *config, const CodexRunRequest *req, size_t *len_out,
                 char **err_out, bool *timed_out, char **stderr_out);
} CodexDriver;

extern const CodexDriver codex_exec_driver;

#endif /* CODEX_DRIVER_H */
