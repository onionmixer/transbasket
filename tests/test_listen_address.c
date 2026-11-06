/* Harness for tests/test_listen_parse.py. One request per stdin line, hex encoded:
 *   A <port> <hex>            -> "ok <mode> <family> <address hex> <port>" | "err <why>"
 *   S <hex>[,<hex>...]        -> "1" | "0"   (the chunks are fed one after another)
 *   P <hex>                   -> "<rc> <key> <scan>" with the real parse_config_line()
 *                                (only when built with -DWITH_CONFIG_LOADER)
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "listen_address.h"

#ifdef WITH_CONFIG_LOADER
#include "../src/config_loader.c"
#endif

static size_t unhex(const char *s, size_t n, char *out) {
    size_t k = 0;
    if (n % 2 != 0) {
        fprintf(stderr, "odd hex length\n");
        exit(2);
    }
    for (size_t i = 0; i < n; i += 2) {
        unsigned v;
        char pair[3] = {s[i], s[i + 1], 0};
        if (sscanf(pair, "%2x", &v) != 1) {
            fprintf(stderr, "bad hex\n");
            exit(2);
        }
        out[k++] = (char)v;
    }
    out[k] = 0;
    return k;
}

static void hex_out(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02x", b[i]);
    }
}

int main(void) {
    static char line[1 << 16], buf[1 << 15];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 'A') {
            char *p = line + 2;
            int port = (int)strtol(p, &p, 10);
            p++;
            unhex(p, strlen(p), buf);
            struct sockaddr_storage ss;
            ListenMode mode = 0;
            const char *why = NULL;
            if (parse_listen_address(buf, port, &ss, &mode, &why) == 0) {
                if (ss.ss_family == AF_INET) {
                    struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
                    printf("ok %d %d ", (int)mode, (int)ss.ss_family);
                    hex_out((const unsigned char *)&s4->sin_addr, 4);
                    printf(" %d\n", ntohs(s4->sin_port));
                } else {
                    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
                    printf("ok %d %d ", (int)mode, (int)ss.ss_family);
                    hex_out(s6->sin6_addr.s6_addr, 16);
                    printf(" %d\n", ntohs(s6->sin6_port));
                }
            } else {
                printf("err %s\n", why ? why : "(null)");
            }
        } else if (line[0] == 'S') {
            ListenScan scan;
            listen_scan_init(&scan);
            char *p = line + 2;
            for (;;) {
                char *comma = strchr(p, ',');
                size_t n = comma ? (size_t)(comma - p) : strlen(p);
                size_t k = unhex(p, n, buf);
                listen_scan_feed(&scan, buf, k);
                if (!comma) {
                    break;
                }
                p = comma + 1;
            }
            printf("%d\n", listen_scan_matched(&scan) ? 1 : 0);
#ifdef WITH_CONFIG_LOADER
        } else if (line[0] == 'P') {
            char *p = line + 2;
            size_t k = unhex(p, strlen(p), buf);
            ListenScan scan;
            listen_scan_init(&scan);
            listen_scan_feed(&scan, buf, k);
            char key[512] = "", value[512] = "";
            int rc = parse_config_line(buf, key, sizeof key, value, sizeof value);
            printf("%d %s %d\n", rc, rc == 1 ? key : "-", listen_scan_matched(&scan) ? 1 : 0);
#endif
        } else {
            fprintf(stderr, "bad request\n");
            return 2;
        }
        fflush(stdout);
    }
    return 0;
}
