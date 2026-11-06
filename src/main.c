/**
 * Main entry point for transbasket C implementation.
 * HTTP-based translation server daemon with signal handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include "config_loader.h"
#include "http_server.h"
#include "trans_cache.h"
#include "utils.h"
#include "daemon.h"

/* Global server instance for signal handler */
static TranslationServer *g_server = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_save_cache = 0;

/* Never take locks, log, or stop MHD from a signal context. */
static void signal_handler(int signum) {
    if (signum == SIGHUP) g_save_cache = 1;
    else g_shutdown = 1;
}

/* Setup signal handlers */
static int setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        LOG_INFO("Error: Failed to setup SIGINT handler");
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        LOG_INFO("Error: Failed to setup SIGTERM handler");
        return -1;
    }

    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        LOG_INFO("Error: Failed to setup SIGHUP handler");
        return -1;
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

/* Print usage information */
static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("HTTP-based translation server daemon\n\n");
    printf("Options:\n");
    printf("  -c, --config PATH       Path to configuration file\n");
    printf("                          (default: transbasket.local.conf, else transbasket.conf)\n");
    printf("  -p, --prompt PATH       Path to prompt prefix file (default: PROMPT_PREFIX.txt)\n");
    printf("  -r, --role PATH         Path to system role file (default: ROLS.txt)\n");
    printf("  -w, --workers NUM       Number of worker threads (default: 30)\n");
    printf("  -d, --daemon            Run in the background; exits 0 once the server listens, 1 on a failed start\n");
    printf("  -l, --log-file PATH     With -d: append the daemon's log to PATH (default: discarded)\n");
    printf("  -h, --help              Show this help message\n\n");
    printf("Environment Variables:\n");
    printf("  TRANSBASKET_CONFIG      Config file path\n");
    printf("  MAX_WORKERS             Thread pool size\n\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s -c /etc/transbasket.conf -w 20\n", program_name);
    printf("  %s -d -l /var/log/transbasket.log -c /etc/transbasket.conf\n", program_name);
    printf("  MAX_WORKERS=20 %s\n\n", program_name);
}

/* Main function */
int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *prompt_prefix_path = NULL;
    const char *system_role_path = NULL;
    int max_workers = 0;
    bool run_as_daemon = false;
    const char *log_file = NULL;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc) {
            prompt_prefix_path = argv[++i];
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--role") == 0) && i + 1 < argc) {
            system_role_path = argv[++i];
        } else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workers") == 0) && i + 1 < argc) {
            const char *w = argv[++i];
            bool plain = true;
            max_workers = atoi_saturating(w, &plain);
            if (!plain) {
                LOG_INFO("Warning: -w '%s' is not a plain integer in int range; read as %d", w, max_workers);
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            run_as_daemon = true;
        } else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log-file") == 0) && i + 1 < argc) {
            log_file = argv[++i];
        } else {
            LOG_INFO("Error: Unknown option: %s", argv[i]);
            printf("\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    if (log_file && !run_as_daemon) {
        LOG_INFO("Error: -l/--log-file is only used with -d (in the foreground the log is stdout)");
        return 1;
    }

    /* Get from environment if not set via command line */
    if (!config_path) {
        config_path = getenv("TRANSBASKET_CONFIG");
    }

    if (max_workers == 0) {
        const char *workers_env = getenv("MAX_WORKERS");
        if (workers_env) {
            bool plain = true;
            max_workers = atoi_saturating(workers_env, &plain);
            if (!plain) {
                LOG_INFO("Warning: MAX_WORKERS '%s' is not a plain integer in int range; read as %d", workers_env, max_workers);
            }
        }
    }

    /* Default max_workers */
    if (max_workers == 0) {
        max_workers = 30;
    }

    /* Print startup banner before daemonizing */
    if (!run_as_daemon) {
        printf("===========================================\n");
        printf("  Transbasket Translation Server (C)\n");
        printf("  Version: 1.0.0\n");
        printf("===========================================\n\n");
    }

    /* Daemonize if requested: only the daemon returns; the launcher waits for its
     * start report and exits 0/1 (PLAN_OPEN_ISSUES.md A). Startup messages below are
     * the same as in the foreground and reach the launcher's terminal. */
    if (run_as_daemon) {
        LOG_INFO("Starting transbasket in daemon mode...");
        if (daemon_begin(log_file) != 0) {
            LOG_INFO("Error: Failed to daemonize process");
            return 1;
        }
    }

    /* Setup signal handlers */
    if (setup_signal_handlers() != 0) {
        LOG_INFO("Error: Failed to setup signal handlers");
        return 1;
    }

    LOG_INFO("Signal handlers initialized");

    /* Load configuration */
    LOG_INFO("Loading configuration...");

    Config *config = load_config(config_path, prompt_prefix_path, system_role_path);
    if (!config) {
        LOG_INFO("Error: Failed to load configuration");
        return 1;
    }

    {
        LOG_INFO("Configuration loaded successfully:");
        LOG_INFO("  Model Type: %s", config->model_type_str ? config->model_type_str : "normal");
        if (config->model_type == MODEL_TYPE_CODEX) {
            /* OPENAI_* may be NULL in codex mode - never %s a NULL */
            LOG_INFO("  Codex Interface: exec");
            LOG_INFO("  Codex Bin: %s", config->codex_bin ? config->codex_bin : "codex");
            LOG_INFO("  Codex Model: %s", config->codex_model ? config->codex_model : "(default)");
            LOG_INFO("  Codex Sandbox: %s", config->codex_sandbox ? config->codex_sandbox : "read-only");
            LOG_INFO("  Codex Timeout: %ds", config->codex_timeout);
        } else {
            LOG_INFO("  Base URL: %s", config->openai_base_url);
            LOG_INFO("  Model: %s", config->openai_model);
        }
        LOG_INFO(strchr(config->listen, ':') ? "  Listen (configured): [%s]:%d" : "  Listen (configured): %s:%d",
                 config->listen, config->port);
        LOG_INFO("  Workers: %d", max_workers);
        printf("\n");
    }

    /* Initialize server */
    LOG_INFO("Initializing translation server...");

    g_server = translation_server_init(config, max_workers);
    if (!g_server) {
        LOG_INFO("Error: Failed to initialize server");
        free_config(config);
        return 1;
    }

    /* Start server */
    if (translation_server_start(g_server) != 0) {
        LOG_INFO("Error: Failed to start server");
        translation_server_free(g_server);
        free_config(config);
        return 1;
    }

    if (run_as_daemon) {
        int ready = g_shutdown ? 1 : daemon_ready(&g_shutdown);   /* checked again just before the report */
        if (ready == 1) {
            LOG_INFO("Stop requested during startup; not reporting the daemon as started");
        } else if (ready != 0) {
            LOG_INFO("Error: could not switch the daemon's output; stopping");
            g_shutdown = 1;
        }
    } else {
        printf("\n===========================================\n");
        printf("  Server is running\n");
        printf("  Press Ctrl+C to stop\n");
        printf("===========================================\n\n");
    }

    /* Main loop - wait for shutdown signal */
    while (!g_shutdown) {
        if (g_save_cache) {
            g_save_cache = 0;
            if (g_server->cache) {
                int rc = trans_cache_save(g_server->cache);
                LOG_INFO("SIGHUP cache save %s", rc == 0 ? "completed" : "failed");
            }
        }
        sleep(1);
    }

    /* Cleanup */
    LOG_INFO("Shutting down server...");

    translation_server_free(g_server);
    g_server = NULL;

    free_config(config);

    LOG_INFO("Server shutdown complete");

    return 0;
}
