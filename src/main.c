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

    fprintf(stderr, "\nReceived signal %s (%d), shutting down gracefully...\n",
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
        fprintf(stderr, "Error: Failed to setup SIGINT handler\n");
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "Error: Failed to setup SIGTERM handler\n");
        return -1;
    }

    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        fprintf(stderr, "Error: Failed to setup SIGHUP handler\n");
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
    printf("  -w, --workers NUM       Number of worker threads (default: 10)\n");
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
        } else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workers") == 0) && i + 1 < argc) {
            max_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            run_as_daemon = true;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n\n", argv[i]);
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
        max_workers = 10;
    }

    /* Print startup banner before daemonizing */
    if (!run_as_daemon) {
        fprintf(stderr, "===========================================\n");
        fprintf(stderr, "  Transbasket Translation Server (C)\n");
        fprintf(stderr, "  Version: 1.0.0\n");
        fprintf(stderr, "===========================================\n\n");
    }

    /* Daemonize if requested */
    if (run_as_daemon) {
        fprintf(stderr, "Starting transbasket in daemon mode...\n");
        if (daemonize() != 0) {
            fprintf(stderr, "Error: Failed to daemonize process\n");
            return 1;
        }
        /* After daemonizing, we can't use stderr anymore */
    }

    /* Setup signal handlers */
    if (setup_signal_handlers() != 0) {
        if (!run_as_daemon) {
            fprintf(stderr, "Error: Failed to setup signal handlers\n");
        }
        return 1;
    }

    if (!run_as_daemon) {
        fprintf(stderr, "Signal handlers initialized\n");
    }

    /* Load configuration */
    if (!run_as_daemon) {
        fprintf(stderr, "Loading configuration...\n");
    }

    Config *config = load_config(config_path, prompt_prefix_path);
    if (!config) {
        if (!run_as_daemon) {
            fprintf(stderr, "Error: Failed to load configuration\n");
        }
        return 1;
    }

    if (!run_as_daemon) {
        fprintf(stderr, "Configuration loaded successfully:\n");
        fprintf(stderr, "  Base URL: %s\n", config->openai_base_url);
        fprintf(stderr, "  Model: %s\n", config->openai_model);
        fprintf(stderr, "  Listen: %s:%d\n", config->listen, config->port);
        fprintf(stderr, "  Workers: %d\n\n", max_workers);
    }

    /* Initialize server */
    if (!run_as_daemon) {
        fprintf(stderr, "Initializing translation server...\n");
    }

    g_server = translation_server_init(config, max_workers);
    if (!g_server) {
        if (!run_as_daemon) {
            fprintf(stderr, "Error: Failed to initialize server\n");
        }
        free_config(config);
        return 1;
    }

    /* Start server */
    if (translation_server_start(g_server) != 0) {
        if (!run_as_daemon) {
            fprintf(stderr, "Error: Failed to start server\n");
        }
        translation_server_free(g_server);
        free_config(config);
        return 1;
    }

    if (!run_as_daemon) {
        fprintf(stderr, "\n===========================================\n");
        fprintf(stderr, "  Server is running\n");
        fprintf(stderr, "  Press Ctrl+C to stop\n");
        fprintf(stderr, "===========================================\n\n");
    }

    /* Main loop - wait for shutdown signal */
    while (!g_shutdown) {
        sleep(1);
    }

    /* Cleanup */
    if (!run_as_daemon) {
        fprintf(stderr, "\nShutting down server...\n");
    }

    translation_server_free(g_server);
    g_server = NULL;

    free_config(config);

    if (!run_as_daemon) {
        fprintf(stderr, "Server shutdown complete\n");
    }

    return 0;
}
