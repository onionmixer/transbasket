/* Harness for tests/test_sse.py: the real handle_streaming_response() (static, so the
 * source is included). One request per stdin line:
 *   S <hex body>          -> "<status> <hex content or ->"
 *   F <k> <hex body>      -> same, with the k-th malloc() inside backend_normal.c failing
 */
#include <stdlib.h>
static long tb_fail_at = -1, tb_mallocs = 0;
static void *tb_malloc(size_t n) {
    if (++tb_mallocs == tb_fail_at) return NULL;
    return malloc(n);
}
#define malloc(n) tb_malloc(n)
#include "../src/backend_normal.c"
#undef malloc

int main(void) {
    static char line[1 << 22];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *p = line + 2;
        tb_fail_at = -1;
        if (line[0] == 'F') {
            tb_fail_at = strtol(p, &p, 10);
            p++;
        }
        size_t n = strlen(p) / 2;
        char *body = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            sscanf(p + 2 * i, "%2x", &v);
            body[i] = (char)v;
        }
        tb_mallocs = 0;
        SseStatus st = SSE_OK;
        char *r = handle_streaming_response(n ? body : "", n, "u", &st);
        printf("%d ", (int)st);
        if (r) {
            for (size_t i = 0; r[i]; i++) printf("%02x", (unsigned char)r[i]);
        } else {
            printf("-");
        }
        printf(" %ld\n", tb_mallocs);
        fflush(stdout);
        free(r);
        free(body);
    }
    return 0;
}
