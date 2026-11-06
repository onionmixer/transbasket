"""Specification of src/listen_address.c (PLAN_LISTEN_ADDRESS.md section 3.1 and 3.3).

parse() takes the literal with the same glibc inet_pton the C code calls (through
ctypes, so both sides see one parser) and classifies the resulting bytes with the
python ipaddress module, an implementation independent of the C range checks.
scan() is a regular expression, independent of the C state machine.
"""
import ctypes
import ipaddress
import random
import re
import socket

_libc = ctypes.CDLL("libc.so.6")
_libc.inet_pton.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p]
_libc.inet_pton.restype = ctypes.c_int

IPV4, IPV6_ONLY, DUAL_STACK = 1, 2, 3

WHY_PORT = "PORT is out of range"
WHY_MULTICAST = "is a multicast address"
WHY_BROADCAST = "is the broadcast address"
WHY_MAPPED = "is an IPv4-mapped address; use the IPv4 form"
WHY_LINK_LOCAL = "is a link-local address, which needs an interface scope that LISTEN does not support"
WHY_NOT_NUMERIC = ("is not a numeric IPv4 or IPv6 address (host names are not resolved; "
                   "use 127.0.0.1 or ::1 for loopback, 0.0.0.0 or :: for all interfaces)")


def pton(family, text):
    size = 4 if family == socket.AF_INET else 16
    buf = ctypes.create_string_buffer(size)
    return buf.raw if _libc.inet_pton(family, text, buf) == 1 else None


def parse(text, port):
    """text: bytes without NUL. -> ("ok", mode, family, address bytes, port) or ("err", why)."""
    assert b"\x00" not in text
    if not 1 <= port <= 65535:
        return ("err", WHY_PORT)
    raw = pton(socket.AF_INET, text)
    if raw is not None:
        a = ipaddress.IPv4Address(raw)
        if a in ipaddress.IPv4Network("224.0.0.0/4"):
            return ("err", WHY_MULTICAST)
        if a == ipaddress.IPv4Address("255.255.255.255"):
            return ("err", WHY_BROADCAST)
        return ("ok", IPV4, socket.AF_INET, raw, port)
    raw = pton(socket.AF_INET6, text)
    if raw is not None:
        a = ipaddress.IPv6Address(raw)
        if a.is_multicast:                                   # ff00::/8
            return ("err", WHY_MULTICAST)
        if a.ipv4_mapped is not None:                        # ::ffff:0:0/96
            return ("err", WHY_MAPPED)
        if a in ipaddress.IPv6Network("fe80::/10"):
            return ("err", WHY_LINK_LOCAL)
        return ("ok", DUAL_STACK if int(a) == 0 else IPV6_ONLY, socket.AF_INET6, raw, port)
    return ("err", WHY_NOT_NUMERIC)


_SCAN = re.compile(rb"(?:[ \t\r\v\f]|\xef\xbb\xbf)*(?:(?i:export)[ \t\r\v\f]+)?(?i:listen)(?![A-Za-z0-9_])")


def scan(line):
    """True when the line (bytes, up to its first LF) tries to set LISTEN."""
    return _SCAN.match(line.split(b"\n", 1)[0]) is not None


# ---- fixed cases ------------------------------------------------------------

ADDR_CASES = [
    b"0.0.0.0", b"127.0.0.1", b"198.18.0.1", b"255.255.255.255", b"255.255.255.254", b"223.255.255.255",
    b"224.0.0.0", b"224.0.0.1", b"239.255.255.255", b"240.0.0.0", b"0.1.2.3", b"192.0.2.1",
    b"::", b"::1", b"0:0:0:0:0:0:0:0", b"0:0:0:0:0:0:0:1", b"::0.0.0.0", b"::127.0.0.1",
    b"::ffff:127.0.0.1", b"::ffff:0.0.0.0", b"::FFFF:7f00:1", b"::fffe:127.0.0.1", b"::1:ffff:127.0.0.1",
    b"ff00::", b"ff02::1", b"feff::", b"fe80::1", b"FE80::1", b"febf:ffff::1", b"fec0::1", b"fe7f::1",
    b"2001:db8::1", b"2001:DB8::1", b"ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255",
    b"1234:5678:9abc:def0:1234:5678:9abc:def0", b"1:2:3:4:5:6:7:8",
    b"127.1", b"0x7f.0.0.1", b"010.0.0.1", b"127.000.000.001", b"999.1.1.1", b"1.2.3", b"1.2.3.4.5",
    b" 127.0.0.1", b"127.0.0.1 ", b"\t::1", b"[::1]", b"fe80::1%lo", b"::1%1", b"localhost",
    b"", b"abc", b"0", b"::1::", b":::", b"1:2:3:4:5:6:7:8:9", b"12345::", b"::g", b"-1.0.0.0",
    b"127.0.0.1/8", b"\xea\xb0\x80", b"\xff", b"1.2.3.04", b"01.2.3.4", b"1.2.3.256",
]

SCAN_CASES = [
    b'LISTEN="127.0.0.1"', b"LISTEN=0.0.0.0", b'  LISTEN="::"', b'LISTEN = "127.0.0.1"', b"LISTEN =127.0.0.1",
    b"LISTEN\t=x", b"export LISTEN=x", b"EXPORT\tLISTEN=x", b"export  listen=x", b"listen=x", b"Listen=x",
    b"LISTEN", b"LISTEN\n", b"LISTEN 127.0.0.1", b"LISTEN: x", b"LISTEN-x", b"LISTEN.x", b'\xef\xbb\xbfLISTEN="x"',
    b"\xef\xbb\xbf\xef\xbb\xbf LISTEN=x", b"\v\fLISTEN=x", b"\r\nLISTEN=x",
    b"#LISTEN=x", b"# LISTEN=x", b"LISTENX=1", b"LISTEN_PORT=1", b"LISTEN0=1", b"exportLISTEN=x", b"export=x",
    b"export", b"export ", b"xLISTEN=x", b"LISTE=x", b"\xef\xbbLISTEN=x", b"\xefLISTEN=x", b"PORT=1 LISTEN=x",
    b"", b"\n", b"LIS\nTEN=x", b"export\nLISTEN=x", b"LISTEN\xef", b"LISTEN\x00", b"L\xc4\xb1STEN=x",
]


# ---- generators -------------------------------------------------------------

_V4_FIRST = [0, 1, 10, 127, 169, 172, 192, 198, 223, 224, 225, 239, 240, 254, 255]
_MUTATE = b"0123456789abcdefABCDEF:.%[] \tx-+g/"


def _v4(r):
    b = [r.choice(_V4_FIRST) if r.random() < 0.6 else r.randrange(256)] + [
        r.choice([0, 1, 255, r.randrange(256)]) for _ in range(3)]
    if r.random() < 0.05:
        b = [255, 255, 255, r.choice([254, 255])]
    style = r.randrange(6)
    if style == 0:
        return ".".join(map(str, b)).encode()
    if style == 1:
        return ".".join(f"{x:03d}" for x in b).encode()            # leading zeros
    if style == 2:
        return ".".join(hex(x) for x in b).encode()
    if style == 3:                                                  # shortened forms
        return ".".join(map(str, b[:r.randrange(1, 4)])).encode()
    if style == 4:
        return str(int.from_bytes(bytes(b), "big")).encode()
    return ".".join(map(str, b + [r.randrange(256)])).encode()


def _v6(r):
    kind = r.randrange(9)
    if kind == 0:
        raw = bytes(16)
    elif kind == 1:
        raw = bytes(15) + bytes([r.choice([0, 1, 2])])
    elif kind == 2:
        raw = bytes(10) + bytes([0xff, 0xff]) + bytes(r.randrange(256) for _ in range(4))
    elif kind == 3:
        raw = bytes(10) + bytes([r.choice([0xff, 0xfe, 0x00]), r.choice([0xff, 0xfe, 0x00])]) + bytes(r.randrange(256) for _ in range(4))
    elif kind == 4:
        raw = bytes([0xff, r.randrange(256)]) + bytes(r.randrange(256) for _ in range(14))
    elif kind == 5:
        raw = bytes([0xfe, r.choice([0x7f, 0x80, 0x9f, 0xa0, 0xbf, 0xc0, 0xff])]) + bytes(r.randrange(2) for _ in range(14))
    elif kind == 6:
        raw = bytes([r.choice([0x00, 0xfe, 0xff, 0x01])]) + bytes(15)
    else:
        raw = bytes(r.randrange(256) for _ in range(16))
    a = ipaddress.IPv6Address(raw)
    style = r.randrange(6)
    if style == 0:
        s = a.compressed
    elif style == 1:
        s = a.exploded
    elif style == 2:
        s = a.compressed.upper()
    elif style == 3:                                                # dotted tail
        s = a.exploded.rsplit(":", 2)[0] + ":" + str(ipaddress.IPv4Address(raw[12:]))
    elif style == 4:
        s = ":".join(f"{int.from_bytes(raw[i:i + 2], 'big'):x}" for i in range(0, 16, 2))
    else:
        s = a.compressed + r.choice(["%lo", "%1", "/64", ""])
    return s.encode()


def gen_address(r):
    s = bytearray(_v4(r) if r.random() < 0.45 else _v6(r))
    for _ in range(r.choice([0, 0, 0, 1, 1, 2])):
        op, pos = r.randrange(4), r.randrange(len(s) + 1)
        if op == 0 and s:
            del s[min(pos, len(s) - 1)]
        elif op == 1:
            s.insert(pos, r.choice(_MUTATE))
        elif op == 2 and s:
            s[min(pos, len(s) - 1)] = r.choice(_MUTATE)
        else:
            s = bytearray(b"[" + bytes(s) + b"]") if r.random() < 0.5 else bytearray(r.choice([b" ", b"\t"]) + bytes(s))
    return bytes(s).replace(b"\x00", b"")


def gen_port(r):
    if r.random() < 0.05:
        return r.choice([0, 65536, -1, 2**31 - 1])
    return r.choice([1, 80, 8889, 65535, r.randrange(1, 65536)])


_SCAN_TOKENS = [b" ", b"\t", b"\r", b"\v", b"\f", b"\xef\xbb\xbf", b"\xef\xbb", b"export", b"EXPORT", b"Export",
                b"listen", b"LISTEN", b"Listen", b"LISTE", b"ISTEN", b"=", b'"', b"_", b"X", b"0", b"-", b"#",
                b"\n", b"\xc4\xb1", b"x", b":", b".", b"127.0.0.1"]


def gen_line(r):
    line = b"".join(r.choice(_SCAN_TOKENS) for _ in range(r.randrange(0, 7)))
    if r.random() < 0.3:
        line = b" " * r.choice([0, 1, 1017, 1022, 1023, 1024, 2000]) + line
    return line.replace(b"\x00", b"")


def split_random(r, data):
    """Random chunking, so the C side is fed in pieces like the config loader does."""
    cuts = sorted(r.sample(range(1, len(data)), min(len(data) - 1, r.randrange(0, 4)))) if len(data) > 1 else []
    out, prev = [], 0
    for c in cuts + [len(data)]:
        out.append(data[prev:c])
        prev = c
    return out or [b""]


def corpus(seed, n):
    r = random.Random(seed)
    return [(gen_address(r), gen_port(r)) for _ in range(n)]
