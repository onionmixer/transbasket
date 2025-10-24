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

/* Global server instance for signal handler */
static TranslationServer *g_server = NULL;
static volatile bool g_shutdown = false;

/* Signal handler for graceful shutdown */
static void signal_handler(int signum) {
    const char *signame = "UNKNOWN";

    switch (signum) {
        case SIGINT:
            signame = "SIGINT";
            break;
        case SIGTERM:
            signame = "SIGTERM";
            break;
        case SIGHUP:
            signame = "SIGHUP";
            break;
    }

    /* Handle SIGHUP specially - save cache without shutdown */
    if (signum == SIGHUP) {
        LOG_INFO("Received signal %s (%d), saving translation cache...",
                signame, signum);

        if (g_server && g_server->cache) {
            if (trans_cache_save(g_server->cache) == 0) {
                LOG_INFO("Translation cache saved successfully");

                /* Print cache statistics */
                size_t total, active, expired;
                trans_cache_stats(g_server->cache, &total, &active, &expired,
                                g_server->config->cache_threshold,
                                g_server->config->cache_cleanup_days);
                LOG_INFO("Cache stats: total=%zu, active=%zu, expired=%zu",
                        total, active, expired);
            } else {
                LOG_INFO("Warning: Failed to save translation cache");
            }
        } else {
            LOG_INFO("Warning: Cache not available");
        }

        return;  /* Don't shutdown on SIGHUP */
    }

    /* For SIGINT/SIGTERM - shutdown gracefully */
    LOG_INFO("Received signal %s (%d), shutting down gracefully...",
            signame, signum);

    g_shutdown = true;

    if (g_server) {
        translation_server_stop(g_server);
    }
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

/* Get environment variable with default */
static const char *getenv_default(const char *name, const char *default_value) {
    const char *value = getenv(name);
    return value ? value : default_value;
}

/* Print usage information */
static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("HTTP-based translation server daemon\n\n");
    printf("Options:\n");
    printf("  -c, --config PATH       Path to configuration file (default: transbasket.conf)\n");
    printf("  -p, --prompt PATH       Path to prompt prefix file (default: PROMPT_PREFIX.txt)\n");
    printf("  -r, --role PATH         Path to system role file (default: ROLS.txt)\n");
    printf("  -w, --workers NUM       Number of worker threads (default: 30)\n");
    printf("  -d, --daemon            Run as daemon in background\n");
    printf("  -h, --help              Show this help message\n\n");
    printf("Environment Variables:\n");
    printf("  TRANSBASKET_CONFIG      Config file path\n");
    printf("  MAX_WORKERS             Thread pool size\n\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s -c /etc/transbasket.conf -w 20\n", program_name);
    printf("  %s -d -c /etc/transbasket.conf\n", program_name);
    printf("  MAX_WORKERS=20 %s\n\n", program_name);
}

/* Main function */
int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *prompt_prefix_path = NULL;
    const char *system_role_path = NULL;
    int max_workers = 0;
    bool run_as_daemon = false;

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
            max_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            run_as_daemon = true;
        } else {
            LOG_INFO("Error: Unknown option: %s", argv[i]);
            printf("\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Get from environment if not set via command line */
    if (!config_path) {
        config_path = getenv("TRANSBASKET_CONFIG");
    }

    if (max_workers == 0) {
        const char *workers_env = getenv("MAX_WORKERS");
        if (workers_env) {
            max_workers = atoi(workers_env);
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

    /* Daemonize if requested */
    if (run_as_daemon) {
        LOG_INFO("Starting transbasket in daemon mode...");
        if (daemonize() != 0) {
            LOG_INFO("Error: Failed to daemonize process");
            return 1;
        }
        /* After daemonizing, we can't use stderr anymore */
    }

    /* Setup signal handlers */
    if (setup_signal_handlers() != 0) {
        if (!run_as_daemon) {
            LOG_INFO("Error: Failed to setup signal handlers");
        }
        return 1;
    }

    if (!run_as_daemon) {
        LOG_INFO("Signal handlers initialized");
    }

    /* Load configuration */
    if (!run_as_daemon) {
        LOG_INFO("Loading configuration...");
    }

    Config *config = load_config(config_path, prompt_prefix_path, system_role_path);
    if (!config) {
        if (!run_as_daemon) {
            LOG_INFO("Error: Failed to load configuration");
        }
        return 1;
    }

    if (!run_as_daemon) {
        LOG_INFO("Configuration loaded successfully:");
        LOG_INFO("  Base URL: %s", config->openai_base_url);
        LOG_INFO("  Model: %s", config->openai_model);
        LOG_INFO("  Listen: %s:%d", config->listen, config->port);
        LOG_INFO("  Workers: %d", max_workers);
        printf("\n");
    }

    /* Initialize server */
    if (!run_as_daemon) {
        LOG_INFO("Initializing translation server...");
    }

    g_server = translation_server_init(config, max_workers);
    if (!g_server) {
        if (!run_as_daemon) {
            LOG_INFO("Error: Failed to initialize server");
        }
        free_config(config);
        return 1;
    }

    /* Start server */
    if (translation_server_start(g_server) != 0) {
        if (!run_as_daemon) {
            LOG_INFO("Error: Failed to start server");
        }
        translation_server_free(g_server);
        free_config(config);
        return 1;
    }

    if (!run_as_daemon) {
        printf("\n===========================================\n");
        printf("  Server is running\n");
        printf("  Press Ctrl+C to stop\n");
        printf("===========================================\n\n");
    }

    /* Main loop - wait for shutdown signal */
    while (!g_shutdown) {
        sleep(1);
    }

    /* Cleanup */
    if (!run_as_daemon) {
        LOG_INFO("Shutting down server...");
    }

    translation_server_free(g_server);
    g_server = NULL;

    free_config(config);

    if (!run_as_daemon) {
        LOG_INFO("Server shutdown complete");
    }

    return 0;
}
