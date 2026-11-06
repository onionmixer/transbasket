/**
 * LISTEN address parsing and LISTEN line recognition (PLAN_LISTEN_ADDRESS.md section 3).
 * Specification: tests/listen_address_ref.py.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include "listen_address.h"

int parse_listen_address(const char *text, int port, struct sockaddr_storage *out,
                         ListenMode *mode, const char **why) {
    const char *dummy;
    if (!why) {
        why = &dummy;
    }
    if (!text || !out || !mode) {
        *why = "missing value";
        return -1;
    }
    if (port < 1 || port > 65535) {
        *why = "PORT is out of range";
        return -1;
    }

    memset(out, 0, sizeof(*out));
    struct in_addr a4;
    struct in6_addr a6;

    if (inet_pton(AF_INET, text, &a4) == 1) {
        const unsigned char *b = (const unsigned char *)&a4;
        if (b[0] >= 224 && b[0] <= 239) {
            *why = "is a multicast address";
            return -1;
        }
        if (b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255) {
            *why = "is the broadcast address";
            return -1;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)out;
        sin->sin_family = AF_INET;
        sin->sin_port = htons((unsigned short)port);
        sin->sin_addr = a4;
        *mode = LISTEN_IPV4;
        return 0;
    }

    if (inet_pton(AF_INET6, text, &a6) == 1) {
        const unsigned char *b = a6.s6_addr;
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        static const unsigned char zero[16] = {0};
        if (b[0] == 0xff) {
            *why = "is a multicast address";
            return -1;
        }
        if (memcmp(b, mapped, sizeof(mapped)) == 0) {
            *why = "is an IPv4-mapped address; use the IPv4 form";
            return -1;
        }
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) {
            *why = "is a link-local address, which needs an interface scope that LISTEN does not support";
            return -1;
        }
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons((unsigned short)port);
        sin6->sin6_addr = a6;
        *mode = memcmp(b, zero, sizeof(zero)) == 0 ? LISTEN_DUAL_STACK : LISTEN_IPV6_ONLY;
        return 0;
    }

    *why = "is not a numeric IPv4 or IPv6 address (host names are not resolved; "
           "use 127.0.0.1 or ::1 for loopback, 0.0.0.0 or :: for all interfaces)";
    return -1;
}

/* ---- LISTEN line recognizer ------------------------------------------------ */

enum {
    SCAN_LEAD,          /* leading blanks and BOMs; pos = bytes of a BOM seen */
    SCAN_EXPORT,        /* inside "export"; pos = letters matched */
    SCAN_EXPORT_GAP,    /* "export" done, at least one blank required */
    SCAN_AFTER_EXPORT,  /* blanks after "export" */
    SCAN_WORD,          /* inside "listen"; pos = letters matched */
    SCAN_WORD_DONE,     /* "listen" done, next byte decides */
    SCAN_YES,
    SCAN_NO
};

static int is_blank(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static int is_ident(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static unsigned char ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

void listen_scan_init(ListenScan *scan) {
    scan->state = SCAN_LEAD;
    scan->pos = 0;
}

static void scan_byte(ListenScan *scan, unsigned char c) {
    static const unsigned char bom[3] = {0xef, 0xbb, 0xbf};
    static const char export_word[] = "export";
    static const char listen_word[] = "listen";

    if (c == '\n') {
        scan->state = scan->state == SCAN_WORD_DONE ? SCAN_YES : SCAN_NO;
        return;
    }
    unsigned char l = ascii_lower(c);
    switch (scan->state) {
    case SCAN_LEAD:
        if (scan->pos > 0) {
            if (c != bom[scan->pos]) {
                scan->state = SCAN_NO;
            } else if (++scan->pos == 3) {
                scan->pos = 0;
            }
        } else if (c == bom[0]) {
            scan->pos = 1;
        } else if (is_blank(c)) {
            /* stay */
        } else if (l == 'e') {
            scan->state = SCAN_EXPORT;
            scan->pos = 1;
        } else if (l == 'l') {
            scan->state = SCAN_WORD;
            scan->pos = 1;
        } else {
            scan->state = SCAN_NO;
        }
        break;
    case SCAN_EXPORT:
        if (l != (unsigned char)export_word[scan->pos]) {
            scan->state = SCAN_NO;
        } else if (++scan->pos == 6) {
            scan->state = SCAN_EXPORT_GAP;
        }
        break;
    case SCAN_EXPORT_GAP:
        scan->state = is_blank(c) ? SCAN_AFTER_EXPORT : SCAN_NO;
        break;
    case SCAN_AFTER_EXPORT:
        if (l == 'l') {
            scan->state = SCAN_WORD;
            scan->pos = 1;
        } else if (!is_blank(c)) {
            scan->state = SCAN_NO;
        }
        break;
    case SCAN_WORD:
        if (l != (unsigned char)listen_word[scan->pos]) {
            scan->state = SCAN_NO;
        } else if (++scan->pos == 6) {
            scan->state = SCAN_WORD_DONE;
        }
        break;
    case SCAN_WORD_DONE:
        scan->state = is_ident(c) ? SCAN_NO : SCAN_YES;
        break;
    default:
        break;
    }
}

void listen_scan_feed(ListenScan *scan, const char *data, size_t len) {
    for (size_t i = 0; i < len && scan->state != SCAN_YES && scan->state != SCAN_NO; i++) {
        scan_byte(scan, (unsigned char)data[i]);
    }
}

bool listen_scan_matched(const ListenScan *scan) {
    return scan->state == SCAN_YES || scan->state == SCAN_WORD_DONE;
}
