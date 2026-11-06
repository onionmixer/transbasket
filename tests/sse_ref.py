"""Specification of handle_streaming_response() in src/http_client.c (PLAN_OPEN_ISSUES.md D/E).

Framing and classification are written here independently of the C code. Whether an
event's data is one JSON value, where it ends and what its members hold is asked of the
installed cJSON 1.7.15 through ctypes (python's json module accepts and rejects
different inputs, e.g. NaN, "1." and leading zeros). The NUL rule is
tests/cjson_nul_ref.decodes_nul() over the consumed bytes.
"""
import ctypes
import random
import re

from cjson_nul_ref import decodes_nul

OK, NUL_ESCAPE, RAW_NUL, BAD_JSON, ERROR_EVENT, BAD_CONTENT, NO_MEMORY = range(7)

_lib = ctypes.CDLL("libcjson.so.1.7.15")
_lib.cJSON_ParseWithOpts.restype = ctypes.c_void_p
_lib.cJSON_ParseWithOpts.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
_lib.cJSON_Delete.argtypes = [ctypes.c_void_p]
for _f in ("cJSON_GetObjectItem",):
    getattr(_lib, _f).restype = ctypes.c_void_p
    getattr(_lib, _f).argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cJSON_GetArrayItem.restype = ctypes.c_void_p
_lib.cJSON_GetArrayItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
for _f in ("cJSON_IsArray", "cJSON_IsNull", "cJSON_IsString"):
    getattr(_lib, _f).restype = ctypes.c_int
    getattr(_lib, _f).argtypes = [ctypes.c_void_p]
_lib.cJSON_GetStringValue.restype = ctypes.c_char_p
_lib.cJSON_GetStringValue.argtypes = [ctypes.c_void_p]

_LINE = re.compile(rb"\r\n|\n|\r")


def _event(data, error_event):
    """-> (status, content bytes)."""
    if error_event:
        return ERROR_EVENT, b""
    if data == b"[DONE]":
        return OK, b""
    buf = ctypes.create_string_buffer(data)
    end = ctypes.c_char_p()
    root = _lib.cJSON_ParseWithOpts(buf, ctypes.byref(end), 0)
    if not root:
        return BAD_JSON, b""
    consumed = ctypes.cast(end, ctypes.c_void_p).value - ctypes.addressof(buf)
    try:
        if data[consumed:].strip(b" \t\r\n"):
            return BAD_JSON, b""
        if decodes_nul(data[:consumed]):
            return NUL_ESCAPE, b""
        err = _lib.cJSON_GetObjectItem(root, b"error")
        if err and not _lib.cJSON_IsNull(err):
            return ERROR_EVENT, b""
        choices = _lib.cJSON_GetObjectItem(root, b"choices")
        first = _lib.cJSON_GetArrayItem(choices, 0) if choices and _lib.cJSON_IsArray(choices) else None
        delta = _lib.cJSON_GetObjectItem(first, b"delta") if first else None
        content = _lib.cJSON_GetObjectItem(delta, b"content") if delta else None
        if not content or _lib.cJSON_IsNull(content):
            return OK, b""
        if not _lib.cJSON_IsString(content):
            return BAD_CONTENT, b""
        return OK, _lib.cJSON_GetStringValue(content) or b""
    finally:
        _lib.cJSON_Delete(root)


def parse(body):
    """bytes -> (status, content or None). None content = nothing collected."""
    if b"\x00" in body:
        return RAW_NUL, None
    out = b""
    ev, has_data, error_event = [], False, False

    def dispatch():
        nonlocal out
        if not error_event and (not has_data or not b"\n".join(ev)):
            return OK          # empty data is not dispatched
        st, c = _event(b"\n".join(ev), error_event)
        out += c
        return st

    if body.startswith(b"\xef\xbb\xbf"):
        body = body[3:]                      # SSE: a leading BOM is skipped
    for line in _LINE.split(body):
        if line == b"":
            st = dispatch()
            if st != OK:
                return st, None
            ev, has_data, error_event = [], False, False
            continue
        if line.startswith(b":"):
            continue
        name, colon, value = line.partition(b":")   # no colon: the whole line is the name, value empty
        if colon and value.startswith(b" "):
            value = value[1:]
        if name == b"data":
            ev.append(value)
            has_data = True
        elif name == b"event":
            error_event = value == b"error"
    st = dispatch()                          # a last event without the blank line is still read (deliberate)
    if st != OK:
        return st, None
    return OK, (out or None)


def delta(content):
    import json
    return b'{"choices":[{"delta":{"content":' + json.dumps(content, ensure_ascii=False).encode() + b"}}]}"


# (body, status, content) by hand.
CASES = [
    (b"data: " + delta("A") + b"\n\ndata: " + delta("B") + b"\n\ndata: [DONE]\n\n", OK, b"AB"),
    (b"data: " + delta("A") + b"\r\n\r\ndata: " + delta("B") + b"\r\n\r\n", OK, b"AB"),
    (b"data: " + delta("A") + b"\r\rdata: " + delta("B"), OK, b"AB"),
    (b"data: " + delta("A") + b"\n\n\x00data: " + delta("B") + b"\n\n", RAW_NUL, None),
    (b"data: " + delta("A") + b'\n\ndata: {"error":{"message":"quota"}}\n\n', ERROR_EVENT, None),
    (b"data: " + delta("A") + b"BROKEN\n\n", BAD_JSON, None),
    (b"data: " + delta("A") + b'\n\nevent: error\ndata: {"x":1}\n\n', ERROR_EVENT, None),
    (b'data: {"choices":[{"delta":{"content":5}}]}\n\n', BAD_CONTENT, None),
    (b'data: {"choices":[{"delta":{"role":"assistant","content":""}}]}\n\ndata: ' + delta("A") + b'\n\n'
     b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\ndata: {"choices":[],"usage":{"total_tokens":3}}\n\n'
     b'data: {"choices":[{"delta":{"content":null}}]}\n\ndata: [DONE]\n\n', OK, b"A"),
    (b": comment\n\ndata: " + delta("OK") + b"\n\n", OK, b"OK"),
    (b'data: {"choices":[{"delta":\ndata: {"content":"A"}}]}\n\n', OK, b"A"),
    (b"data:" + delta("A") + b"\n\n", OK, b"A"),
    (b'data: {"error":null,"choices":[{"delta":{"content":"A"}}]}\n\n', OK, b"A"),
    (b"data: [DONE]\n\n", OK, None), (b"", OK, None), (b"data: \n\n", OK, None), (b"data:\ndata:\n\n", BAD_JSON, None),
    (b'data: {"choices":[{"delta":{"content":"a\\u0000b"}}]}\n\n', NUL_ESCAPE, None),
    (b"\xef\xbb\xbfdata: " + delta("A") + b"\n\ndata: " + delta("B") + b"\n\n", OK, b"AB"),      # leading BOM
    (b"data: " + delta("A") + b"\n\n\xef\xbb\xbfdata: " + delta("B") + b"\n\n", OK, b"A"),     # BOM only at the start
    (b"event: error\nevent\ndata: " + delta("A") + b"\n\n", OK, b"A"),                     # colonless event resets
    (b"data\ndata: " + delta("A") + b"\n\n", OK, b"A"),                                     # colonless data: "" + LF + JSON
    (b"data: " + delta("A") + b"\n\ndata: " + delta("B"), OK, b"AB"),                        # last event without blank line
]

TOKENS = [b"data: ", b"data:", b"data\n", b"event\n", b"\xef\xbb\xbf", b"event: error\n", b"event: x\n", b": c\n", b"\n", b"\r\n", b"\r", b"\n\n", b"\r\n\r\n",
          b"[DONE]", b"BROKEN", b"\x00", b'{"error":{"m":1}}', b'{"error":null}', b'{"choices":[]}',
          b'{"choices":[{"delta":{}}]}', b'{"choices":[{"delta":{"content":null}}]}', b'{"choices":[{"delta":{"content":1}}]}',
          b'{"choices":[{"delta":{"content":"\\u0000"}}]}', b'{"choices":[{"delta":{"content":"\\u00e9"}}]}', b"{",
          b'{"choices":[{"delta":{"content":"x"}}]}', delta("가"), delta("A"), delta(""), b" ", b"1", b"1."]


def generate(r):
    return b"".join(r.choice(TOKENS) for _ in range(r.randrange(0, 12)))


def corpus(seed, n):
    r = random.Random(seed)
    return [generate(r) for _ in range(n)]
