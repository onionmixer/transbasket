/* Harness for tests/test_timestamp.py: one hex-encoded string per stdin line,
 * prints 1 or 0 = validate_timestamp(). The input is malloc'ed at its exact size
 * (plus the NUL) so ASan sees any read past it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

int main(void) {
    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        size_t n = strlen(line) / 2;
        char *s = malloc(n + 1);
        if (!s) return 2;
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            if (sscanf(line + 2 * i, "%2x", &v) != 1) return 2;
            s[i] = (char)v;
        }
        s[n] = '\0';
        printf("%d\n", validate_timestamp(s) ? 1 : 0);
        free(s);
    }
    return 0;
}
