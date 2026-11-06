"""Specification of strip_emoji_and_shortcodes() and unescape_string() in src/utils.c
(PLAN_OPEN_ISSUES.md C, Q4). Independent of the C code: UTF-8 validity comes from
python's own decoder, the shortcode rule is a regular expression.

In the case tables "~" stands for a backslash.
"""
import re

EMOJI = [(0x1F300, 0x1F5FF), (0x1F600, 0x1F64F), (0x1F680, 0x1F6FF), (0x1F700, 0x1F77F), (0x1F780, 0x1F7FF),
         (0x1F800, 0x1F8FF), (0x1F900, 0x1F9FF), (0x1FA00, 0x1FA6F), (0x1FA70, 0x1FAFF), (0x2700, 0x27BF),
         (0x2600, 0x26FF)]
SPACE = b" \t\n\v\f\r"
SHORTCODE = re.compile(rb"(?<![A-Za-z0-9]):(?=[a-z0-9_+-]*[a-z])[a-z0-9_+-]+:(?![A-Za-z0-9])")


def char_at(b, i):
    """(length, codepoint or None for an invalid byte) by python's strict decoder."""
    c = b[i]
    n = 1 if c < 0x80 else 2 if 0xC0 <= c < 0xE0 else 3 if 0xE0 <= c < 0xF0 else 4 if 0xF0 <= c < 0xF8 else 0
    if n == 1:
        return 1, c
    if n:
        try:
            return n, ord(b[i:i + n].decode("utf-8"))
        except UnicodeDecodeError:
            pass
    return 1, None


def strip(b):
    """bytes (no NUL) -> bytes."""
    out = bytearray()
    i, last_space = 0, False
    while i < len(b):
        if b[i] == ord(":"):
            m = SHORTCODE.match(b, i)
            if m and (i == 0 or not chr(b[i - 1]).isascii() or not chr(b[i - 1]).isalnum()):
                i = m.end()
                continue
        n, cp = char_at(b, i)
        if cp is not None and any(lo <= cp <= hi for lo, hi in EMOJI):
            i += n
            continue
        if b[i] in SPACE:
            if b[i] == ord("\n"):
                out += b"\n"
                last_space = False
            elif not last_space and out:
                out += b" "
                last_space = True
            i += 1
            continue
        out += b[i:i + n]
        i += n
        last_space = False
    if out.endswith(b" "):
        out = out[:-1]
    return bytes(out)


_ESC = {ord("n"): b"\n", ord("t"): b"\t", ord("r"): b"\r", ord("\\"): b"\\", ord('"'): b'"', ord("'"): b"'"}


def unescape(b):
    out = bytearray()
    i = 0
    while i < len(b):
        if b[i] == 0x5C and i + 1 < len(b) and b[i + 1] in _ESC:
            out += _ESC[b[i + 1]]
            i += 2
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def bs(s):
    return s.encode("utf-8").replace(b"~", b"\\")


# (input, strip result) written by hand; checked against strip() and the C code.
STRIP_CASES = [
    ("Note: this is it", "Note: this is it"), ("at 10:30 today", "at 10:30 today"),
    ("see http://example.com/a", "see http://example.com/a"), ("ratio 3:1", "ratio 3:1"), ("a:b:c", "a:b:c"),
    ("Q: 왜? A: 이유", "Q: 왜? A: 이유"), ("x :) y", "x :) y"), ("emoji :smile: ok", "emoji ok"),
    ("[2001:db8::1]", "[2001:db8::1]"), ("10:30:45", "10:30:45"), (":+1: yes", ":+1: yes"), (":Smile:", ":Smile:"),
    ("word:smile:", "word:smile:"), ("/:id:/", "//"), ("가:smile:나", "가나"), (":a::b:", ""), ("::smile::", "::"),
    (":smile:x", ":smile:x"), ("hi 😀 there", "hi there"), ("☀ sun", "sun"), ("tab\there  two  spaces", "tab here two spaces"),
    ("line1\nline2", "line1\nline2"), ("  lead", "lead"), ("trail  ", "trail"), ("a \n b", "a \n b"), ("", ""),
]
STRIP_RAW = [  # bytes cases: invalid UTF-8 is kept byte for byte, never read past the end
    (b"\xf0", b"\xf0"), (b"a\xf0", b"a\xf0"), (b"\x80", b"\x80"), (b"\xf0\x82\x98\x80", b"\xf0\x82\x98\x80"),
    (b"\xed\xa0\x80", b"\xed\xa0\x80"), (b"\xf4\x90\x80\x80", b"\xf4\x90\x80\x80"), (b"\xc0\x80", b"\xc0\x80"),
    (b"\xe2\x98", b"\xe2\x98"), (b"\xe2\x98\x80", b""), (b"\xff:smile:", b"\xff"),
]
UNESCAPE_CASES = [("a~nb", "a\nb"), ("a~~nb", "a~nb"), ("~t~r~'~\"", "\t\r'\""), ("~x", "~x"), ("~", "~"), ("~~", "~")]
