/* Harness for tests/test_strip.py. One request per stdin line:
 *   <S|U> <cap or -> <hex>   ->  "<rc> <hex output>"
 * S = strip_emoji_and_shortcodes, U = unescape_string. Input and output are malloc'ed
 * at their exact sizes (cap "-" = strlen(input) + 1) so ASan sees any overrun. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

int main(void) {
    static char line[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char mode = line[0];
        char *p = line + 2;
        char *sp = strchr(p, ' ');
        if (!sp) return 2;
        *sp = 0;
        const char *hex = sp + 1;
        size_t n = strlen(hex) / 2;
        char *in = malloc(n + 1);
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            if (sscanf(hex + 2 * i, "%2x", &v) != 1) return 2;
            in[i] = (char)v;
        }
        in[n] = 0;
        size_t cap = strcmp(p, "-") == 0 ? strlen(in) + 1 : (size_t)strtoul(p, NULL, 10);
        char *out = malloc(cap ? cap : 1);
        int rc = mode == 'S' ? strip_emoji_and_shortcodes(in, out, cap) : unescape_string(in, out, cap);
        printf("%d ", rc);
        if (rc == 0) {
            for (size_t i = 0; out[i]; i++) printf("%02x", (unsigned char)out[i]);
        }
        printf("\n");
        fflush(stdout);
        free(in);
        free(out);
    }
    return 0;
}
