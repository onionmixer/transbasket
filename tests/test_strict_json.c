/*
 * Harness for tests/test_strict_json.py: reads records (u32 LE length + bytes)
 * from stdin and prints one strict_json_reason() name per record.
 */
#include <stdio.h>
#include <stdlib.h>
#include "strict_json.h"

int main(void) {
    unsigned char lenbuf[4];
    while (fread(lenbuf, 1, 4, stdin) == 4) {
        size_t len = (size_t)lenbuf[0] | (size_t)lenbuf[1] << 8 | (size_t)lenbuf[2] << 16 | (size_t)lenbuf[3] << 24;
        unsigned char *data = malloc(len ? len : 1);
        if (!data || (len && fread(data, 1, len, stdin) != len)) {
            fprintf(stderr, "truncated record\n");
            free(data);
            return 2;
        }
        printf("%s\n", strict_json_reason(strict_json_validate(data, len)));
        free(data);
    }
    return 0;
}
