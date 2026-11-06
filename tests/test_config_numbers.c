/* Harness for tests/test_config_numbers.py: load_config(<conf>, NULL, <role>) and print the
 * numeric settings, or LOAD_FAIL. Log lines (warnings/errors) go to stdout before it. */
#include <stdio.h>
#include "config_loader.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Config *c = load_config(argv[1], NULL, argv[2]);
    if (!c) {
        printf("LOAD_FAIL\n");
        return 1;
    }
    printf("RESULT port=%d seed=%d temperature=%.17g top_p=%.17g frequency_penalty=%.17g presence_penalty=%.17g "
           "threshold=%d codex_timeout=%d codex_concurrency=%d\n", c->port, c->seed, c->temperature, c->top_p,
           c->frequency_penalty, c->presence_penalty, c->cache_threshold, c->codex_timeout, c->codex_max_concurrency);
    free_config(c);
    return 0;
}
