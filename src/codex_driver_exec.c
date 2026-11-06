/**
 * The `codex exec` driver of the codex backend (PLAN_BACKEND_MODULES.md step 2):
 * maps Config + CodexRunRequest onto CodexExecOptions. The probe and its startup
 * lines below moved here verbatim from the codex backend's init.
 */

#include <stdlib.h>
#include <stdbool.h>
#include "codex_driver.h"
#include "codex_exec.h"
#include "utils.h"

typedef struct {
    bool ephemeral;   /* installed `codex exec` supports --ephemeral */
} ExecState;

static int exec_init(const Config *config, void **state, CodexDriverCaps *caps) {
    ExecState *st = calloc(1, sizeof(*st));
#ifdef TRANSBASKET_TEST_HOOKS
    /* Test builds only: the state allocation fails (decision S1). */
    if (getenv("TB_TEST_FAIL_DRIVER_INIT")) { free(st); st = NULL; }
#endif
    if (!st) {
        LOG_INFO("Error: Memory allocation failed");
        return -1;
    }
    /* The codex CLI surface changes between releases (0.154.0 dropped
     * `mcp-server` and every request started failing with "stdin is not a
     * terminal"), so check `codex exec` and its flags once and say so loudly
     * instead of discovering it per request. Warn only: codex may be
     * installed or upgraded after the daemon starts. */
    CodexCliInfo cli;
    codex_cli_probe(config->codex_bin, &cli);
    st->ephemeral = cli.has_ephemeral;
    caps->structured_output = cli.has_output_schema;
    /* version and server names come from the codex child's output */
    char version_esc[sizeof(cli.version) * 4 + 4];
    log_escape(cli.version[0] ? cli.version : "version unknown", sizeof(cli.version), version_esc, sizeof(version_esc));
    LOG_INFO("codex CLI: %s (interface=exec)\n", version_esc);
    if (!cli.version[0]) {
        LOG_INFO("Warning: `%s --version` failed - is codex installed and on PATH?\n", config->codex_bin);
    }
    if (!cli.has_exec) {
        LOG_INFO("Warning: `%s exec` is not available; codex translations will fail\n", config->codex_bin);
    }
    if (config->codex_ignore_user_config && !cli.has_ignore_user_config) {
        LOG_INFO("Warning: CODEX_IGNORE_USER_CONFIG is set but `%s exec` has no --ignore-user-config; "
                "codex translations will fail\n", config->codex_bin);
    }
    bool isolated = config->codex_ignore_user_config;
    if (!isolated && cli.mcp_enabled > 0) {
        /* sandbox=read-only covers shell commands, not MCP tool calls. */
        char names_esc[sizeof(cli.mcp_names) * 4 + 4];
        log_escape(cli.mcp_names, sizeof(cli.mcp_names), names_esc, sizeof(names_esc));
        LOG_INFO("Warning: codex will start %d MCP server(s) from its config.toml on every "
                "translation (%s); their tools are reachable from untrusted text and are not "
                "covered by the read-only sandbox. Set CODEX_IGNORE_USER_CONFIG=\"true\" "
                "(with CODEX_MODEL) to isolate translations.\n", cli.mcp_enabled, names_esc);
    }

    if (config->source_hint_mode == SOURCE_HINT_MODEL && !cli.has_output_schema) {
        LOG_INFO("Warning: SOURCE_HINT_MODE=\"model\" needs a codex exec with "
                "--output-schema (not advertised by this CLI); source hints come from the script classifier\n");
    }

    *state = st;
    return 0;
}

static void exec_free(void *state) {
    free(state);
}

static char *exec_run(void *state, const Config *cfg, const CodexRunRequest *req, size_t *len_out,
                      char **err_out, bool *timed_out, char **stderr_out) {
    const ExecState *st = state;
    CodexExecOptions opt = {
        .bin = cfg->codex_bin,
        .model = cfg->codex_model,
        .sandbox = cfg->codex_sandbox,
        .approval_policy = cfg->codex_approval_policy,
        .reasoning_effort = cfg->codex_reasoning_effort,
        .cwd = req->cwd,
        .ephemeral = st->ephemeral,
        .ignore_user_config = cfg->codex_ignore_user_config,
        .timeout_sec = cfg->codex_timeout,
        .control = req->control,
        .output_schema = req->schema_path,
        .require_output_file = req->schema_path != NULL,
    };
    return codex_exec_run(&opt, req->prompt, len_out, err_out, timed_out, stderr_out);
}

const CodexDriver codex_exec_driver = {
    .name = "exec",
    .init = exec_init,
    .free = exec_free,
    .run = exec_run,
};
