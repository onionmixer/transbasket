/* V8 of PLAN_BACKEND_MODULES.md: the codex backend works with any driver.
 *
 *   make test-codex-backend
 *
 * Built with -DTRANSBASKET_TEST_HOOKS against every server source except main.c, so
 * codex_backend_init() picks up `codex_test_driver`. A fake driver returns each outcome
 * a real driver can return. For every translation the backend must
 * - call the driver exactly once, holding one concurrency slot while it runs and
 *   giving it back afterwards,
 * - map the outcome to the right result text, status and retryable flag,
 * - have created the per-request temp cwd (and the schema file inside it) before the
 *   call and removed both afterwards,
 * - pass a schema path exactly when structured mode applies (auto source,
 *   SOURCE_HINT_MODE=model and caps.structured_output), ask for the stderr tail only
 *   with DEBUG, and hand its TranslationControl through unchanged.
 * Also: a driver init failure fails openai_translator_init() without calling the
 * driver's free, openai_translator_free() calls it exactly once otherwise, and the real
 * exec driver fails init only when its state cannot be allocated (decision S1).
 * Real deadlines and cancellation are covered by make test-coalescing (exec stages),
 * slot exhaustion by tests/test_backend_parity.py.
 */
#include <dirent.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "codex_driver.h"
#include "config_loader.h"
#include "http_client.h"
#include "language_table.h"

extern const CodexDriver *codex_test_driver;

enum { OK, FAIL, TIMEOUT, NOERR, NULBYTE, EMPTY, FENCED, STRUCT_OK, STRUCT_BAD, STRUCT_NUL };
static int g_mode, g_caps, g_init_fails, g_inits, g_frees, g_runs;
static OpenAITranslator *g_t;               /* to read the semaphore while the fake runs */
static char g_prompt[65536], g_cwd[4096], g_schema[4096];
static int g_cwd_existed, g_schema_existed, g_had_schema, g_sem_in_run, g_stderr_asked;
static const TranslationControl *g_control;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int fake_init(const Config *c, void **state, CodexDriverCaps *caps) {
    (void)c;
    g_inits++;
    if (g_init_fails) return -1;
    caps->structured_output = g_caps;
    *state = malloc(1);
    return 0;
}

static void fake_free(void *state) { g_frees++; free(state); }

static char *dup_len(const char *s, size_t n, size_t *len) {
    char *b = malloc(n + 1);
    memcpy(b, s, n);
    b[n] = '\0';
    *len = n;
    return b;
}

static char *fake_run(void *state, const Config *c, const CodexRunRequest *r, size_t *len, char **err,
                      bool *timed_out, char **stderr_out) {
    (void)state; (void)c;
    struct stat st;
    g_runs++;
    sem_getvalue(&g_t->codex_sem, &g_sem_in_run);
    snprintf(g_prompt, sizeof g_prompt, "%s", r->prompt);
    snprintf(g_cwd, sizeof g_cwd, "%s", r->cwd ? r->cwd : "");
    g_cwd_existed = r->cwd && stat(r->cwd, &st) == 0 && S_ISDIR(st.st_mode);
    g_had_schema = r->schema_path != NULL;
    snprintf(g_schema, sizeof g_schema, "%s", r->schema_path ? r->schema_path : "");
    g_schema_existed = r->schema_path && stat(r->schema_path, &st) == 0;
    g_control = r->control;
    g_stderr_asked = stderr_out != NULL;
    if (stderr_out) *stderr_out = strdup("fake stderr tail");
    switch (g_mode) {
    case OK:         return dup_len("번역 결과", strlen("번역 결과"), len);
    case FAIL:       *err = strdup("fake failure"); return NULL;
    case TIMEOUT:    *err = strdup("fake timed out"); *timed_out = true; return NULL;
    case NOERR:      return NULL;
    case NULBYTE:    return dup_len("a\0b", 3, len);
    case EMPTY:      return dup_len(" \n\t ", 4, len);
    case FENCED:     return dup_len("```\n번역 결과\n```", strlen("```\n번역 결과\n```"), len);
    case STRUCT_OK: {
        const char *s = "{\"source_language\":\"jpn\",\"mixed\":false,\"translation\":\"tomorrow\"}";
        return dup_len(s, strlen(s), len);
    }
    case STRUCT_BAD: return dup_len("{not json", 9, len);
    case STRUCT_NUL: {
        const char s[] = "{\"source_language\":\"jpn\",\"mixed\":false,\"translation\":\"to\0morrow\"}";
        return dup_len(s, sizeof s - 1, len);
    }
    }
    return NULL;
}

static const CodexDriver FAKE = { "fake", fake_init, fake_free, fake_run };

static int dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d);
    return n;
}

typedef struct {
    const char *name;
    int mode;
    const char *from;
    const char *want_text;    /* NULL = the translation must fail */
    int want_status, want_retry, want_schema;
    const char *want_hint;    /* model hint expected in hint_out, or NULL */
} Row;

static void row(OpenAITranslator *t, const char *run_dir, int conc, const Row *w, const TranslationControl *ctl) {
    int before = failures;
    g_mode = w->mode;
    g_runs = 0;
    g_prompt[0] = g_cwd[0] = g_schema[0] = '\0';
    g_cwd_existed = g_schema_existed = g_had_schema = g_stderr_asked = 0;
    g_sem_in_run = -1;
    g_control = NULL;
    TranslationError e = { 0 };
    SourceHint hint;
    char *r = openai_translate(t, w->from, "eng", "明日の会議", "550e8400-e29b-41d4-a716-446655440001",
                               "2026-11-06T01:02:03.456Z", &e, ctl, &hint);
    int sem = -1;
    sem_getvalue(&t->codex_sem, &sem);
    CHECK(g_runs == 1, "%s: driver called %d times", w->name, g_runs);
    if (w->want_text) {
        CHECK(r && strcmp(r, w->want_text) == 0, "%s: result \"%s\", want \"%s\"", w->name, r ? r : "NULL", w->want_text);
    } else {
        CHECK(r == NULL, "%s: result \"%s\", want failure", w->name, r);
        CHECK(e.status_code == w->want_status, "%s: status %d, want %d", w->name, e.status_code, w->want_status);
        CHECK(e.retryable == w->want_retry, "%s: retryable %d", w->name, e.retryable);
        CHECK(e.message != NULL, "%s: no error message", w->name);
    }
    if (w->want_hint) {
        CHECK(strcmp(hint.lang, w->want_hint) == 0 && hint.origin == SOURCE_HINT_ORIGIN_MODEL,
              "%s: hint %s/%c, want %s from the model", w->name, hint.lang, hint.origin ? hint.origin : '-', w->want_hint);
    }
    CHECK(g_sem_in_run == conc - 1, "%s: semaphore %d while the driver ran, want %d", w->name, g_sem_in_run, conc - 1);
    CHECK(sem == conc, "%s: semaphore %d after the call, want %d", w->name, sem, conc);
    CHECK(g_cwd_existed, "%s: temp cwd did not exist during the run", w->name);
    CHECK(dir_entries(run_dir) == 0, "%s: %d entries left in CODEX_RUN_DIR", w->name, dir_entries(run_dir));
    CHECK(g_had_schema == w->want_schema, "%s: schema path %s, want %s", w->name, g_had_schema ? "set" : "none",
          w->want_schema ? "set" : "none");
    if (g_had_schema) {
        struct stat st;
        CHECK(g_schema_existed, "%s: schema file missing during the run", w->name);
        CHECK(stat(g_schema, &st) != 0, "%s: schema file left behind", w->name);
        CHECK(strncmp(g_schema, g_cwd, strlen(g_cwd)) == 0 && g_schema[strlen(g_cwd)] == '/',
              "%s: schema outside the temp cwd", w->name);
    }
    CHECK(g_stderr_asked == t->config->debug, "%s: stderr tail asked %d with DEBUG=%d", w->name, g_stderr_asked,
          t->config->debug);
    CHECK(g_control == ctl, "%s: control not handed through", w->name);
    CHECK(strstr(g_prompt, "明日の会議") != NULL, "%s: source text not in the prompt", w->name);
    printf("  %s %s\n", failures == before ? "ok  " : "FAIL", w->name);
    free(r);
    free(e.message);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <work dir> <language_tables.conf>\n", argv[0]); return 2; }
    const char *w = argv[1];
    if (chdir(w) != 0) { perror("chdir"); return 2; }   /* DEBUG traces go to ./trace */
    char conf[4096], run_dir[4096], path[4096];
    snprintf(run_dir, sizeof run_dir, "%s/run", w);
    mkdir(run_dir, 0700);
    snprintf(path, sizeof path, "%s/p.txt", w);
    FILE *f = fopen(path, "w"); fputs("Translate to [TARGET LANGUAGE]", f); fclose(f);
    snprintf(path, sizeof path, "%s/r.txt", w);
    f = fopen(path, "w"); fputs("role", f); fclose(f);
    snprintf(conf, sizeof conf, "%s/t.conf", w);
    f = fopen(conf, "w");
    fprintf(f, "MODEL_TYPE=\"codex\"\nCODEX_BIN=\"/nonexistent/codex\"\nCODEX_RUN_DIR=\"%s\"\nPROMPT_PREFIX_FILE=\"p.txt\"\n"
               "LANGUAGE_TABLE_FILE=\"%s\"\nSOURCE_HINT_MODE=\"model\"\nTRANS_CACHE_TYPE=\"text\"\n"
               "CODEX_TIMEOUT=\"2\"\n", run_dir, argv[2]);   /* a leaked slot then costs 2 s per call, not 100 */
    fclose(f);
    Config *cfg = load_config(conf, NULL, "r.txt");
    LanguageTable *lt = language_table_load(argv[2]);
    if (!cfg || !lt) { printf("FAIL setup\n"); free_config(cfg); language_table_free(lt); return 1; }
    codex_test_driver = &FAKE;

    atomic_bool cancelled = false;
    TranslationControl ctl;
    clock_gettime(CLOCK_MONOTONIC, &ctl.deadline);
    ctl.deadline.tv_sec += 60;
    ctl.cancelled = &cancelled;

    const Row common[] = {
        { "ok, explicit source",          OK,      "jpn", "번역 결과", 0, 0, 0, NULL },
        { "failure -> 502",               FAIL,    "jpn", NULL, 502, 0, 0, NULL },
        { "deadline -> 504 retryable",    TIMEOUT, "jpn", NULL, 504, 1, 0, NULL },
        { "NULL without message -> 502",  NOERR,   "jpn", NULL, 502, 0, 0, NULL },
        { "NUL byte in output -> 502",    NULBYTE, "jpn", NULL, 502, 0, 0, NULL },
        { "whitespace only -> 502",       EMPTY,   "jpn", NULL, 502, 0, 0, NULL },
        { "whole-output fence unwrapped", FENCED,  "jpn", "번역 결과", 0, 0, 0, NULL },
    };
    const Row with_caps[] = {
        { "auto, structured ok",                  STRUCT_OK,  "auto", "tomorrow", 0, 0, 1, "jpn" },
        { "auto, structured invalid -> 502",      STRUCT_BAD, "auto", NULL, 502, 0, 1, NULL },
        { "auto, structured with a NUL -> 502",   STRUCT_NUL, "auto", NULL, 502, 0, 1, NULL },
    };
    const Row no_caps = { "auto, no structured caps -> plain", OK, "auto", "번역 결과", 0, 0, 0, NULL };
    const Row script_mode = { "auto, SOURCE_HINT_MODE=script -> plain", OK, "auto", "번역 결과", 0, 0, 0, NULL };

    for (int caps = 0; caps <= 1; caps++) {
        g_caps = caps;
        g_inits = g_frees = 0;
        int conc = 2;
        OpenAITranslator *t = openai_translator_init(cfg, lt, 3, 60, conc);
        CHECK(t != NULL && g_inits == 1, "init (caps=%d)", caps);
        if (!t) break;
        g_t = t;
        CHECK(t->codex_structured_output == caps, "caps not taken over");
        printf(" caps.structured_output=%d\n", caps);
        for (size_t i = 0; i < sizeof common / sizeof common[0]; i++) row(t, run_dir, conc, &common[i], NULL);
        if (caps) {
            for (size_t i = 0; i < sizeof with_caps / sizeof with_caps[0]; i++) row(t, run_dir, conc, &with_caps[i], NULL);
            cfg->source_hint_mode = SOURCE_HINT_SCRIPT;
            row(t, run_dir, conc, &script_mode, NULL);
            cfg->source_hint_mode = SOURCE_HINT_MODEL;
            const Row ctl_row = { "control handed through, DEBUG asks for stderr", OK, "jpn", "번역 결과", 0, 0, 0, NULL };
            cfg->debug = true;
            row(t, run_dir, conc, &ctl_row, &ctl);
            cfg->debug = false;
        } else {
            row(t, run_dir, conc, &no_caps, NULL);
        }
        openai_translator_free(t);
        g_t = NULL;
        CHECK(g_frees == 1, "driver free called %d times", g_frees);
    }

    int before = failures;
    g_init_fails = 1;
    g_inits = g_frees = 0;
    OpenAITranslator *t = openai_translator_init(cfg, lt, 3, 60, 2);
    CHECK(t == NULL && g_inits == 1 && g_frees == 0, "driver init failure: translator %p, inits %d, frees %d",
          (void *)t, g_inits, g_frees);
    openai_translator_free(t);
    printf("  %s driver init failure fails translator init, driver free not called\n", failures == before ? "ok  " : "FAIL");

    /* The real exec driver: S1 (state allocation fails -> init fails), and with a
     * missing CLI init still succeeds (warnings only), as before step 2. */
    before = failures;
    codex_test_driver = NULL;
    setenv("TB_TEST_FAIL_DRIVER_INIT", "1", 1);
    t = openai_translator_init(cfg, lt, 3, 60, 2);
    CHECK(t == NULL, "exec driver: state allocation failure must fail init");
    openai_translator_free(t);
    unsetenv("TB_TEST_FAIL_DRIVER_INIT");
    t = openai_translator_init(cfg, lt, 3, 60, 2);
    CHECK(t != NULL && t->codex_driver == &codex_exec_driver, "exec driver: init with a missing CLI must succeed");
    openai_translator_free(t);
    printf("  %s exec driver: S1 allocation failure fails init; missing CLI only warns\n", failures == before ? "ok  " : "FAIL");

    language_table_free(lt);
    free_config(cfg);
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
