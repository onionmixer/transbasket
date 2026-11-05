"""Specification of json_decodes_nul() (src/strict_json.c) and an oracle that
asks the installed cJSON 1.7.15 itself whether it produced a NUL
(PLAN_CLEANUP_DAYS_AND_NUL.md sections 4 and 5).

In the case tables below "~" stands for a backslash, so that no escape sequence
in this file can be decoded by an editor or a tool that handles the source.
"""
import ctypes
import random

BACKSLASH = 0x5C
HEX = frozenset(b"0123456789abcdefABCDEF")


def bs(text):
    """Case notation -> bytes: '~' is a backslash."""
    return text.encode("utf-8").replace(b"~", bytes([BACKSLASH]))


def decodes_nul(data):
    """The specification. True when some escape would decode to U+0000."""
    end = data.find(b"\x00")
    if end >= 0:
        data = data[:end]                     # cJSON reads a C string
    i = 0
    while i < len(data):
        if data[i] != BACKSLASH:
            i += 1
            continue
        if i + 1 < len(data) and data[i + 1] == ord("u"):
            quad = data[i + 2:i + 6]
            if len(quad) < 4 or any(c not in HEX for c in quad) or int(quad, 16) == 0:
                return True
            i += 6
        else:
            i += 2                            # the byte after a backslash never starts an escape
    return False


# (case, verdict of the specification). Verdicts were written by hand from the
# rules; tests/test_cjson_nul.py also checks each one against the oracle.
CASES = [
    ('{"k":"a~u0000b"}', True), ('{"k":"a~u00ZZb"}', True), ('{"k":"a~u12ZZb"}', True),
    ('{"k":"a~uZZ12b"}', True), ('{"k":"a~u000Zb"}', True), ('{"k":"a~u-123b"}', True),
    ('{"k":"a~u+123b"}', True), ('{"k":"a~u 123b"}', True), ('{"k":"a~uD8ZZb"}', True),
    ('{"k":"a~u0041b"}', False), ('{"k":"a~u00e9b"}', False), ('{"k":"a~u00E9b"}', False),
    ('{"k":"a~u0001b"}', False), ('{"k":"a~uD83D~uDE00b"}', False), ('{"k":"~~u0000"}', False),
    ('{"k":"~~~u0000"}', True), ('{"k":"~~~~u0000"}', False), ('{"te~u0000xt":"v"}', True),
    ('{"text~u0000junk":"v"}', True), ('["a",["b",{"c":"~u0000"}]]', True), ('"~u0000"', True),
    ('{"k":"~u0041~u0000"}', True), ('{"k":"~"~u0000"}', True), ('{"k":"~/~b~f~n~r~t"}', False),
    ('{"k":"v"} ~u0000', True), ('~u0000', True), ('~u', True), ('~u0', True), ('~u00', True),
    ('~u000', True), ('~u0001', False), ('~', False), ('~x', False), ('', False), ('abc', False),
    ('{"k":"~U0000"}', False), ('~U00ZZ', False), ('{"k":"가나다"}', False), ('\ufeff{"k":"~u0000"}', True), ('{"k":"~uFFFF"}', False),
]
RAW = [  # cases that need raw bytes
    (b'{"k":"a\x00\\u0000"}', False),        # stops at the raw NUL
    (b'\\u00\x0000', True),                  # NUL inside the four bytes: fewer than four remain
    (b'{"k":"a"}\x00\\u0000', False),
    (b'\\u0041\x00\\u0000', False),
]


def cases():
    return [(bs(c), v) for c, v in CASES] + RAW


# ---- oracle: the real library -------------------------------------------------

class _Node(ctypes.Structure):
    pass


_Node._fields_ = [("next", ctypes.POINTER(_Node)), ("prev", ctypes.POINTER(_Node)),
                  ("child", ctypes.POINTER(_Node)), ("type", ctypes.c_int),
                  ("valuestring", ctypes.c_void_p), ("valueint", ctypes.c_int),
                  ("valuedouble", ctypes.c_double), ("string", ctypes.c_void_p)]

_MALLOC = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t)
_FREE = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class _Hooks(ctypes.Structure):
    _fields_ = [("malloc_fn", _MALLOC), ("free_fn", _FREE)]


class Oracle:
    """cJSON 1.7.15 with an allocator that fills every block with 0xFF and records
    its size. parse_string() allocates (raw length - escapes + 1) bytes and writes
    the decoded bytes and a terminating NUL, so a 0x00 before the terminator means
    the string was cut. Only blocks referenced by valuestring/string are examined
    (nodes come from the same allocator and are zero-filled by cJSON)."""

    def __init__(self, path="libcjson.so.1.7.15"):
        self.libc = ctypes.CDLL("libc.so.6")
        self.libc.malloc.restype, self.libc.malloc.argtypes = ctypes.c_void_p, [ctypes.c_size_t]
        self.libc.free.argtypes = [ctypes.c_void_p]
        self.sizes = {}

        def alloc(size):
            p = self.libc.malloc(size)
            if p:
                ctypes.memset(p, 0xFF, size)
                self.sizes[p] = size
            return p

        def release(p):
            self.sizes.pop(p, None)
            self.libc.free(p)

        self._alloc, self._release = _MALLOC(alloc), _FREE(release)   # keep the callbacks alive
        self.lib = ctypes.CDLL(path)
        self.lib.cJSON_InitHooks.argtypes = [ctypes.POINTER(_Hooks)]
        self._hooks = _Hooks(self._alloc, self._release)
        self.lib.cJSON_InitHooks(ctypes.byref(self._hooks))
        self.lib.cJSON_ParseWithOpts.restype = ctypes.POINTER(_Node)
        self.lib.cJSON_ParseWithOpts.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
        self.lib.cJSON_Delete.argtypes = [ctypes.POINTER(_Node)]
        assert ctypes.sizeof(_Node) == 64

    def _cut(self, ptr):
        size = self.sizes[ptr]                 # KeyError = not a block from parse_string
        buf = ctypes.string_at(ptr, size)
        t = size - 1
        while t >= 0 and buf[t] == 0xFF:
            t -= 1
        if t < 0 or buf[t] != 0:
            raise AssertionError(f"no terminator in {buf!r}")
        return 0 in buf[:t]

    def parse(self, data):
        """None if cJSON rejects data; else (any string or key cut, bytes consumed).
        Inputs must not contain 0xFF (it is the fill byte)."""
        assert b"\xff" not in data
        buf = ctypes.create_string_buffer(data)
        end = ctypes.c_char_p()
        root = self.lib.cJSON_ParseWithOpts(buf, ctypes.byref(end), 0)
        if not root:
            return None
        consumed = ctypes.cast(end, ctypes.c_void_p).value - ctypes.addressof(buf)
        cut, stack = False, [root]
        while stack:
            node = stack.pop()
            while node:
                c = node.contents
                for p in (c.valuestring, c.string):
                    if p and self._cut(p):
                        cut = True
                if c.child:
                    stack.append(c.child)
                node = c.next
        self.lib.cJSON_Delete(root)
        return cut, consumed


# ---- generator ----------------------------------------------------------------

TOKENS = [bs(t) for t in [
    "a", "가", "~\"", "~~", "~/", "~n", "~b", "~u0041", "~u00e9", "~u00E9", "~u0000", "~u00ZZ", "~uZZ00",
    "~uD83D~uDE00", "~uD800", "~uDC00", "~uD83D~u0000", "~u", "~u0", "~u00", "~u000", "~", "~x", "~0",
    "~~u0000", "~~~u0000", "~u~\"ab", "~u12\"3", "~u-123", "\"", "~uFFFF", "~u0001", "~U0000", "~U0041"]] + [b"\x00", b"\xef\xbb\xbf", b"\x01"]


def _string(r):
    return b'"' + b"".join(r.choice(TOKENS) for _ in range(r.randrange(0, 5))) + b'"'


def _value(r, depth=0):
    x = r.random()
    if depth < 3 and x < 0.25:
        return b"{" + b",".join(_string(r) + b":" + _value(r, depth + 1) for _ in range(r.randrange(0, 3))) + b"}"
    if depth < 3 and x < 0.45:
        return b"[" + b",".join(_value(r, depth + 1) for _ in range(r.randrange(0, 3))) + b"]"
    if x < 0.55:
        return r.choice([b"1", b"null", b"true", b"-0.5e3"])
    return _string(r)


def generate(r):
    doc = (b"\xef\xbb\xbf" if r.random() < 0.1 else b"") + (b" " if r.random() < 0.2 else b"") + _value(r)
    if r.random() < 0.2:
        doc += r.choice([b" ", bs(" ~u0000"), bs("junk~"), bs('"~u00ZZ"'), b"\x00" + bs("~u0000")])
    b = bytearray(doc)
    for _ in range(r.randrange(0, 3)):
        pos, op = r.randrange(len(b) + 1), r.randrange(3)
        ch = r.choice(bs('~u0Z"{}[]:,a') + b"\x00")
        if op == 0 and b:
            del b[min(pos, len(b) - 1)]
        elif op == 1:
            b.insert(pos, ch)
        elif b:
            b[min(pos, len(b) - 1)] = ch
    return bytes(b).replace(b"\xff", b"")


def corpus(seed, n):
    r = random.Random(seed)
    return [generate(r) for _ in range(n)]
