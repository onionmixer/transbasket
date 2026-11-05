#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <microhttpd.h>
#include <stdbool.h>
#include <pthread.h>
#include "config_loader.h"
#include "http_client.h"
#include "language_table.h"
#include "trans_cache.h"
#include "inflight_translation.h"

/* Translation server structure */
typedef struct {
    Config *config;
    LanguageTable *lang_table;
    OpenAITranslator *translator;
    struct MHD_Daemon *daemon;
    int max_workers;
    InflightManager *inflight;

    /* Cache components */
    TransCache *cache;
    pthread_t cache_bg_thread;
    atomic_bool cache_bg_running;
} TranslationServer;

/* Initialize translation server */
TranslationServer *translation_server_init(Config *config, int max_workers);

/* Start translation server */
int translation_server_start(TranslationServer *server);

/* Stop translation server */
void translation_server_stop(TranslationServer *server);

/* Free translation server */
void translation_server_free(TranslationServer *server);

#endif /* HTTP_SERVER_H */
