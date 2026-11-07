/* PLAN_FAULT_INJECTION.md F2 and Q2: allocation failures while the codex backend
 * builds its prompt, and while build_instruction_message() substitutes placeholders.
 *
 *   make test-fault-codex          (TEST_SANITIZERS='-fsanitize=address,undefined ...' for leaks)
 *
 * src/prompt_builder.c and src/backend_codex.c are included here with malloc, calloc,
 * realloc, strdup and strndup replaced by counting wrappers, so only allocations made
 * in those two files are counted; other translation units (the facade, logging, the
 * language table, cJSON, uuid) are not. One successful call counts N allocations; then
 * every k = 1..N is failed once ("single") and from k on ("persistent" - reaches the
 * allocations made only after a failure, such as the error message itself).
 *
 * Every call must end in one of two ways:
 * - the driver was not called: NULL, status 0 (HTTP 502), not retryable;
 * - the driver was called: its prompt holds the source text and no placeholder that
 *   should have been substituted, and the result is either the baseline result or a
 *   failure.
 * and always: every concurrency slot is back, nothing is left in CODEX_RUN_DIR, and
 * (under ASan) nothing leaks. build_instruction_message() itself, as the normal
 * backend calls it (text given): NULL or fully substituted.
 */
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include "codex_driver.h"
#include "codex_exec.h"
#include "config_loader.h"
#include "http_client.h"
#include "language_table.h"
#include "strict_json.h"
#include "source_hint.h"
#include "utils.h"
#include "backend_codex.h"
#include "prompt_builder.h"
#include "trace_file.h"

/* ---- counting allocator for the two included files ---- */
static int tb_armed;              /* count only while a call under test runs */
static long tb_fail_at = -1;      /* <= 0: never fail */
static int tb_persist;            /* fail from tb_fail_at on, not only there */
static long tb_count;
static int tb_should_fail(void) {
    if (!tb_armed) return 0;
    long n = ++tb_count;
    if (tb_fail_at <= 0) return 0;
    return tb_persist ? n >= tb_fail_at : n == tb_fail_at;
}
__attribute__((unused)) static void *tb_malloc(size_t n) { return tb_should_fail() ? NULL : malloc(n); }
__attribute__((unused)) static void *tb_calloc(size_t a, size_t b) { return tb_should_fail() ? NULL : calloc(a, b); }
__attribute__((unused)) static void *tb_realloc(void *p, size_t n) { return tb_should_fail() ? NULL : realloc(p, n); }
__attribute__((unused)) static char *tb_strdup(const char *s) { return tb_should_fail() ? NULL : strdup(s); }
__attribute__((unused)) static char *tb_strndup(const char *s, size_t n) { return tb_should_fail() ? NULL : strndup(s, n); }
#define malloc(n) tb_malloc(n)
#define calloc(a, b) tb_calloc(a, b)
#define realloc(p, n) tb_realloc(p, n)
#define strdup(s) tb_strdup(s)
#define strndup(s, n) tb_strndup(s, n)
#include "../src/prompt_builder.c"
#include "../src/backend_codex.c"
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *SOURCE = "明日の会議";
static const char *PLACEHOLDERS[] = { "{SOURCE_LANG}", "{SOURCE_CODE}", "{TARGET_LANG}", "{TARGET_CODE}",
                                      "[TARGET LANGUAGE]", "{{LANGUAGE_TO}}", "{TEXT}" };

/* ---- fake driver (outside the counting macros) ---- */
static int g_caps = 1, g_runs;
static OpenAITranslator *g_t;
static char g_prompt[65536];
static int fake_init(const Config *c, void **state, CodexDriverCaps *caps) {
    (void)c;
    caps->structured_output = g_caps;
    *state = malloc(1);
    return *state ? 0 : -1;
}
static void fake_free(void *state) { free(state); }
static char *fake_run(void *state, const Config *c, const CodexRunRequest *r, size_t *len, char **err,
                      bool *timed_out, char **stderr_out) {
    (void)state; (void)c; (void)err; (void)timed_out;
    g_runs++;
    snprintf(g_prompt, sizeof g_prompt, "%s", r->prompt);
    if (stderr_out) *stderr_out = NULL;
    const char *out = r->schema_path
        ? "{\"source_language\":\"jpn\",\"mixed\":false,\"translation\":\"tomorrow's meeting\"}"
        : "tomorrow's meeting";
    char *b = malloc(strlen(out) + 1);
    if (!b) return NULL;
    strcpy(b, out);
    *len = strlen(out);
    return b;
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

static const char *leftover_placeholder(const char *s) {
    for (size_t i = 0; i < sizeof PLACEHOLDERS / sizeof PLACEHOLDERS[0]; i++)
        if (strstr(s, PLACEHOLDERS[i])) return PLACEHOLDERS[i];
    return NULL;
}

typedef struct { long calls, setup_failures, driver_calls, later_failures, null_messages; } Tally;

/* One translation through the facade with the k-th allocation failing (k <= 0: none). */
static char *one(OpenAITranslator *t, const char *from, long k, int persist, TranslationError *e, long *count) {
    memset(e, 0, sizeof *e);
    g_runs = 0;
    g_prompt[0] = '\0';
    SourceHint hint;
    tb_fail_at = k; tb_persist = persist; tb_count = 0; tb_armed = 1;
    char *r = openai_translate(t, from, "eng", SOURCE, "550e8400-e29b-41d4-a716-446655440001",
                               "2026-11-07T01:02:03.456Z", e, NULL, &hint);
    tb_armed = 0;
    *count = tb_count;
    return r;
}

static void sweep(OpenAITranslator *t, const char *run_dir, int conc, const char *name, const char *from) {
    int before = failures;
    TranslationError e;
    long n;
    char *base = one(t, from, -1, 0, &e, &n);
    CHECK(base != NULL && g_runs == 1, "%s: baseline failed (%s)", name, e.message ? e.message : "?");
    char *want = base ? strdup(base) : NULL;
    free(base);
    free(e.message);
    Tally tally = { 0 };
    for (int persist = 0; persist <= 1; persist++) {
        for (long k = 1; k <= n; k++) {
            long seen;
            char *r = one(t, from, k, persist, &e, &seen);
            tally.calls++;
            const char *mode = persist ? "persistent" : "single";
            CHECK(seen >= k, "%s %s k=%ld: only %ld allocations, the failure was never injected", name, mode, k, seen);
            if (g_runs == 0) {
                tally.setup_failures++;
                CHECK(r == NULL, "%s %s k=%ld: result without a driver call", name, mode, k);
                CHECK(e.status_code == 0 && !e.retryable, "%s %s k=%ld: status %d retryable %d, want 0/0",
                      name, mode, k, e.status_code, e.retryable);
                if (!e.message) tally.null_messages++;
                CHECK(e.message || persist, "%s %s k=%ld: no error message although only one allocation failed",
                      name, mode, k);
            } else {
                tally.driver_calls++;
                const char *left = leftover_placeholder(g_prompt);
                CHECK(left == NULL, "%s %s k=%ld: the driver got a prompt with %s left in it", name, mode, k, left);
                CHECK(strstr(g_prompt, SOURCE) != NULL, "%s %s k=%ld: the source text is not in the prompt", name, mode, k);
                if (r) {
                    CHECK(want && strcmp(r, want) == 0, "%s %s k=%ld: result \"%s\", want \"%s\"", name, mode, k, r,
                          want ? want : "?");
                } else {
                    tally.later_failures++;
                    CHECK(e.status_code == 0 || e.status_code == 502, "%s %s k=%ld: status %d after the call",
                          name, mode, k, e.status_code);
                }
            }
            CHECK(slot_gate_free(&t->codex_slots) == conc, "%s %s k=%ld: %d of %d slots free", name, mode, k,
                  slot_gate_free(&t->codex_slots), conc);
            CHECK(dir_entries(run_dir) == 0, "%s %s k=%ld: %d entries left in CODEX_RUN_DIR", name, mode, k,
                  dir_entries(run_dir));
            free(r);
            free(e.message);
        }
    }
    printf("  %s %s: %ld allocations, %ld failing calls -> %ld setup failures (%ld without a message), "
           "%ld reached the driver (%ld failed afterwards)\n", failures == before ? "ok  " : "FAIL", name, n,
           tally.calls, tally.setup_failures, tally.null_messages, tally.driver_calls, tally.later_failures);
    free(want);
}

/* build_instruction_message() as the normal backend calls it (with the text). Every
 * allocation it makes is needed for the result, so every failing call must be NULL. */
static void builder_sweep(OpenAITranslator *t, const char *name, const char *from, int want_text) {
    int before = failures;
    bool single = false;
    tb_fail_at = -1; tb_count = 0; tb_armed = 1;
    char *base = build_instruction_message(t, from, "eng", SOURCE, &single, false);
    tb_armed = 0;
    long n = tb_count;
    CHECK(base && !leftover_placeholder(base), "%s: baseline %s", name, base ? "has a placeholder" : "failed");
    CHECK(!want_text || (base && strstr(base, SOURCE)), "%s: baseline without the text", name);
    CHECK(single == (want_text != 0), "%s: is_single_message %d, want %d", name, single, want_text != 0);
    long nulls = 0;
    for (int persist = 0; persist <= 1; persist++) {
        for (long k = 1; k <= n; k++) {
            tb_fail_at = k; tb_persist = persist; tb_count = 0; tb_armed = 1;
            char *r = build_instruction_message(t, from, "eng", SOURCE, &single, false);
            tb_armed = 0;
            if (!r) { nulls++; continue; }
            const char *left = leftover_placeholder(r);
            CHECK(left == NULL, "%s %s k=%ld: instruction with %s left in it", name, persist ? "persistent" : "single",
                  k, left);
            CHECK(strcmp(r, base) == 0, "%s %s k=%ld: instruction differs from the baseline", name,
                  persist ? "persistent" : "single", k);
            free(r);
        }
    }
    CHECK(nulls == 2 * n, "%s: %ld of %ld failing calls returned an instruction", name, 2 * n - nulls, 2 * n);
    printf("  %s build_instruction_message %s: %ld allocations, %ld of %ld failing calls returned NULL\n",
           failures == before ? "ok  " : "FAIL", name, n, nulls, 2 * n);
    free(base);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <work dir> <language_tables.conf>\n", argv[0]); return 2; }
    const char *w = argv[1];
    if (chdir(w) != 0) { perror("chdir"); return 2; }
    char run_dir[4096], conf[4096];
    snprintf(run_dir, sizeof run_dir, "%s/run", w);
    mkdir(run_dir, 0700);
    write_file("legacy.txt", "Translate to [TARGET LANGUAGE] ({{LANGUAGE_TO}}).");
    write_file("gemma.txt", "From {SOURCE_LANG} ({SOURCE_CODE}) to {TARGET_LANG} ({TARGET_CODE}):\n{TEXT}");
    write_file("r.txt", "role");
    codex_test_driver = &FAKE;
    const char *templates[] = { "legacy.txt", "gemma.txt" };
    for (int ti = 0; ti < 2; ti++) {
        snprintf(conf, sizeof conf, "%s/t%d.conf", w, ti);
        FILE *f = fopen(conf, "w");
        if (!f) { perror(conf); return 2; }
        fprintf(f, "MODEL_TYPE=\"codex\"\nCODEX_BIN=\"/nonexistent/codex\"\nCODEX_RUN_DIR=\"%s\"\nPROMPT_PREFIX_FILE=\"%s\"\n"
                   "LANGUAGE_TABLE_FILE=\"%s\"\nSOURCE_HINT_MODE=\"model\"\nTRANS_CACHE_TYPE=\"text\"\nCODEX_TIMEOUT=\"2\"\n",
                run_dir, templates[ti], argv[2]);
        fclose(f);
        Config *cfg = load_config(conf, NULL, "r.txt");
        LanguageTable *lt = language_table_load(argv[2]);
        if (!cfg || !lt) { printf("FAIL setup\n"); return 1; }
        int conc = 2;
        OpenAITranslator *t = openai_translator_init(cfg, lt, 3, 60, conc);
        if (!t) { printf("FAIL init\n"); return 1; }
        g_t = t;
        printf(" template %s\n", templates[ti]);
        sweep(t, run_dir, conc, "explicit source (jpn)", "jpn");
        sweep(t, run_dir, conc, "auto, structured", "auto");
        cfg->source_hint_mode = SOURCE_HINT_SCRIPT;
        sweep(t, run_dir, conc, "auto, plain", "auto");
        cfg->source_hint_mode = SOURCE_HINT_MODEL;
        builder_sweep(t, "explicit source (jpn)", "jpn", ti == 1);
        builder_sweep(t, "auto", "auto", 0);
        openai_translator_free(t);
        language_table_free(lt);
        free_config(cfg);
    }
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
