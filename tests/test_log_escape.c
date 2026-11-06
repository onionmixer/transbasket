/* Harness for tests/test_log_safety.py: "<max_in> <out_size> <hex>" -> "v<hex of log_escape output>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

int main(void) {
    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        size_t max_in = strtoul(p, &p, 10);
        size_t out_size = strtoul(p, &p, 10);
        p++;
        size_t n = strlen(p) / 2;
        char *in = malloc(n + 1);
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            sscanf(p + 2 * i, "%2x", &v);
            in[i] = (char)v;
        }
        in[n] = 0;
        char *out = malloc(out_size);
        log_escape(in, max_in, out, out_size);
        printf("v");
        for (size_t i = 0; out[i]; i++) printf("%02x", (unsigned char)out[i]);
        printf("\n");
        free(in);
        free(out);
    }
    return 0;
}
