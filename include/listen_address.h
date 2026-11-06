#ifndef LISTEN_ADDRESS_H
#define LISTEN_ADDRESS_H

/* LISTEN handling (PLAN_LISTEN_ADDRESS.md section 3). Pure functions: no DNS,
 * no sockets. The python specification is tests/listen_address_ref.py. */

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

typedef enum {
    LISTEN_IPV4 = 1,        /* sockaddr_in, no extra MHD flag */
    LISTEN_IPV6_ONLY = 2,   /* sockaddr_in6, MHD_USE_IPv6 */
    LISTEN_DUAL_STACK = 3   /* "::" only: sockaddr_in6, MHD_USE_DUAL_STACK */
} ListenMode;

/* Numeric IPv4/IPv6 literal -> socket address with the port set.
 * Returns 0, or -1 with *why set to a static message. */
int parse_listen_address(const char *text, int port, struct sockaddr_storage *out,
                         ListenMode *mode, const char **why);

/* Recognizes a configuration line that tries to set LISTEN, so that a malformed
 * or overlong one stops startup instead of leaving the 0.0.0.0 default.
 * Shape: leading [ \t\r\v\f] and UTF-8 BOMs, optional "export" + [ \t\r\v\f]+,
 * the word LISTEN (ASCII case-insensitive) not followed by [A-Za-z0-9_].
 * Feed the line in any number of pieces; a '\n' ends it. Constant state. */
typedef struct {
    int state;
    int pos;
} ListenScan;

void listen_scan_init(ListenScan *scan);
void listen_scan_feed(ListenScan *scan, const char *data, size_t len);
bool listen_scan_matched(const ListenScan *scan);

#endif /* LISTEN_ADDRESS_H */
