"""Reference specification of the structured-output validator
(PLAN_SOURCE_HINT_METADATA.md section 4.3).

    python3 -I tests/strict_json_ref.py            # fixed cases
    python3 -I tests/strict_json_ref.py --fuzz     # cross-check against Python's json module

cJSON 1.7.15 accepts invalid input (invalid \\u escapes decoded as NUL, control
bytes, raw LF in strings, numbers like 01, a BOM) and its default lookup is
case-insensitive, so the C validator checks the raw bytes against this
specification BEFORE cJSON sees them; tests compare the two directly.

This is a hand-written scanner on purpose. Python's json module is not used as
the normative oracle because it collapses duplicate keys before values can be
checked, limits integers to 4300 digits, recurses without bound and has no
notion of the depth rule below. It is used only as an independent cross-check
of plain RFC 8259 syntax (--fuzz).

Rules, applied in scan order; the first violation decides the reason:
  utf8       the whole input must be strict UTF-8 (checked first, see source_hint_ref.decode)
  syntax     RFC 8259 grammar; whitespace is exactly space, TAB, LF, CR; no BOM;
             string bytes < 0x20 forbidden; escapes \\" \\\\ \\/ \\b \\f \\n \\r \\t and
             \\u + exactly 4 hex digits; numbers -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
             kept as lexemes (no size limit); literals true/false/null only;
             one value, then only whitespace
  nul        a \\u escape decodes to U+0000 (in any key or string)
  surrogate  a \\uD800-DBFF escape whose next 6 characters are not exactly \\u + 4 hex
             digits in DC00-DFFF (reported at the high surrogate, before anything
             after it is examined: \\ud800\\uZZZZ and \\ud800 at end of input are
             "surrogate"), or a lone \\uDC00-DFFF
  depth      more than MAX_DEPTH arrays/objects open at once (the root object is 1;
             scalars and keys add nothing)
Then, on the root (semantic layer):
  root         the root value is not an object
  duplicate    a reserved key occurs more than once among the ROOT members,
               compared exactly on the decoded key
  translation  the root has no "translation" member, or it is not a string
Every member of every object is validated, duplicates included, before anything
is collapsed.
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from source_hint_ref import INVALID, decode  # noqa: E402

MAX_DEPTH = 32
CROSSCHECK_DEPTH = 200   # extras=False (test-only syntax mode): recursion guard, not an RFC rule
RESERVED = ("translation", "source_language", "mixed")
WS = " \t\n\r"
HEX = "0123456789abcdefABCDEF"


class Reject(Exception):
    pass


class Number(str):
    """A number lexeme, never converted. Not a JSON string (see validate)."""


class Obj(list):
    """A JSON object: the list of ALL its (key, value) pairs, duplicates kept."""


class _Scanner:
    def __init__(self, text, extras):
        self.s, self.i, self.depth, self.extras = text, 0, 0, extras

    def ws(self):
        while self.i < len(self.s) and self.s[self.i] in WS:
            self.i += 1

    def peek(self):
        return self.s[self.i] if self.i < len(self.s) else ""

    def expect(self, ch):
        if self.peek() != ch:
            raise Reject("syntax")
        self.i += 1

    def value(self):
        c = self.peek()
        if c == "{" or c == "[":
            self.depth += 1
            if self.depth > (MAX_DEPTH if self.extras else CROSSCHECK_DEPTH):
                raise Reject("depth")
            result = self.obj() if c == "{" else self.arr()
            self.depth -= 1
            return result
        if c == '"':
            return self.string()
        if c == "-" or c.isdigit() and c in "0123456789":
            return self.number()
        for word, val in (("true", True), ("false", False), ("null", None)):
            if self.s.startswith(word, self.i):
                self.i += len(word)
                return val
        raise Reject("syntax")

    def obj(self):
        self.expect("{")
        pairs = Obj()
        self.ws()
        if self.peek() == "}":
            self.i += 1
            return pairs
        while True:
            self.ws()
            if self.peek() != '"':
                raise Reject("syntax")
            key = self.string()
            self.ws()
            self.expect(":")
            self.ws()
            pairs.append((key, self.value()))
            self.ws()
            if self.peek() == ",":
                self.i += 1
                continue
            self.expect("}")
            return pairs

    def arr(self):
        self.expect("[")
        items = []
        self.ws()
        if self.peek() == "]":
            self.i += 1
            return items
        while True:
            self.ws()
            items.append(self.value())
            self.ws()
            if self.peek() == ",":
                self.i += 1
                continue
            self.expect("]")
            return items

    def hex4(self):
        h = self.s[self.i:self.i + 4]
        if len(h) != 4 or any(ch not in HEX for ch in h):
            raise Reject("syntax")
        self.i += 4
        return int(h, 16)

    def string(self):
        self.expect('"')
        out = []
        while True:
            if self.i >= len(self.s):
                raise Reject("syntax")
            c = self.s[self.i]
            self.i += 1
            if c == '"':
                return "".join(out)
            if ord(c) < 0x20:
                raise Reject("syntax")
            if c != "\\":
                out.append(c)
                continue
            e = self.peek()
            self.i += 1
            simple = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
            if e in simple:
                out.append(simple[e])
                continue
            if e != "u":
                raise Reject("syntax")
            cp = self.hex4()
            if cp == 0 and self.extras:
                raise Reject("nul")
            if 0xD800 <= cp <= 0xDBFF:
                # Pair only if the next 6 characters are exactly \\u + 4 hex digits in
                # DC00-DFFF. Otherwise this high surrogate is the violation, reported
                # here without examining what follows (so \\ud800\\uZZZZ is "surrogate").
                nxt = self.s[self.i:self.i + 6]
                if (len(nxt) == 6 and nxt[:2] == "\\u" and all(ch in HEX for ch in nxt[2:])
                        and 0xDC00 <= int(nxt[2:], 16) <= 0xDFFF):
                    self.i += 6
                    low = int(nxt[2:], 16)
                    out.append(chr(0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00)))
                    continue
                if self.extras:
                    raise Reject("surrogate")
            elif 0xDC00 <= cp <= 0xDFFF and self.extras:
                raise Reject("surrogate")
            out.append(chr(cp))

    def number(self):
        start = self.i
        if self.peek() == "-":
            self.i += 1
        c = self.peek()
        if c == "0":
            self.i += 1
        elif c and c in "123456789":
            while self.peek() and self.peek() in "0123456789":
                self.i += 1
        else:
            raise Reject("syntax")
        if self.peek() == ".":
            self.i += 1
            if not (self.peek() and self.peek() in "0123456789"):
                raise Reject("syntax")
            while self.peek() and self.peek() in "0123456789":
                self.i += 1
        if self.peek() and self.peek() in "eE":
            self.i += 1
            if self.peek() and self.peek() in "+-":
                self.i += 1
            if not (self.peek() and self.peek() in "0123456789"):
                raise Reject("syntax")
            while self.peek() and self.peek() in "0123456789":
                self.i += 1
        return Number(self.s[start:self.i])


def parse(data: bytes, extras=True):
    """Syntax + (optionally) the extra rules. Returns the root value; objects are
    Obj lists of (key, value) pairs, arrays are lists, numbers are Number lexemes."""
    cps = list(decode(data))
    if INVALID in cps:
        raise Reject("utf8")
    sc = _Scanner("".join(map(chr, cps)), extras)
    sc.ws()
    root = sc.value()
    sc.ws()
    if sc.i != len(sc.s):
        raise Reject("syntax")
    return root


def validate(data: bytes):
    """Full rule set. Returns {reserved key: value} for the root, or raises Reject."""
    root = parse(data)
    if not isinstance(root, Obj):
        raise Reject("root")
    keys = [k for k, _ in root]
    for name in RESERVED:
        if keys.count(name) > 1:
            raise Reject("duplicate")
    members = {k: v for k, v in root if k in RESERVED}
    if type(members.get("translation")) is not str:      # Number is a str subclass
        raise Reject("translation")
    return members


CASES = [  # (input bytes, expected: "accept" or the reject reason)
    (b'{"translation":"ok","source_language":"kor","mixed":false}', "accept"),
    (b'{"translation":"ok"} \r\n\t', "accept"),
    (b'{"translation":"a\\\\u0000"}', "accept"),            # literal backslash + u0000
    (b'{"translation":"\\ud83d\\ude00"}', "accept"),         # valid surrogate pair
    (b'{"translation":"\\ud800\\udc00"}', "accept"),         # U+10000
    (b'{"Translation":"x","translation":"y"}', "accept"),    # different keys; lookup is exact
    (b'{"\\u0074ranslation":"via escape"}', "accept"),       # decodes to the reserved key
    (b'{"translation":"\\u2028\xe2\x80\xa9\x7f\xef\xbb\xbf"}', "accept"),  # U+2028/2029, DEL, U+FEFF inside a string
    (b'{"translation":"ok","x":-0,"y":1E+2,"z":1e400}', "accept"),
    (b'{"translation":"ok","x":' + b'1' * 10000 + b'}', "accept"),          # no numeric size limit
    (b'{"translation":"ok","x":1e' + b'9' * 10000 + b'}', "accept"),
    (b'{"translation":"' + b'a' * 100000 + b'"}', "accept"),
    (b'{"translation":"ok","x":' + b'[' * 31 + b']' * 31 + b'}', "accept"),  # 32 open
    (b'{"translation":"ok","x":' + b'[' * 31 + b'0' + b']' * 31 + b'}', "accept"),  # scalars add no depth
    (b'{"translation":"ok","x":' + b'[' * 32 + b']' * 32 + b'}', "depth"),   # 33 open
    (b'{"translation":"ok","x":' + b'[' * 10000 + b'0' + b']' * 10000 + b'}', "depth"),  # no recursion blow-up
    (b'{"translation":"before\\uZZZZafter"}', "syntax"),     # cJSON decodes this to "before"
    (b'{"translation\\uZZZZjunk":"WRONG"}', "syntax"),       # cJSON lets this hijack translation
    (b'\x01{"translation":"ok"}', "syntax"),
    (b'{\x01"translation":"ok"}', "syntax"),
    (b'{"translation":"ok"}\x01', "syntax"),
    (b'\x0b{"translation":"ok"}', "syntax"),
    (b'{"translation":"a\nb"}', "syntax"),
    (b'{"translation":"ok","x":01}', "syntax"),
    (b'{"translation":"ok","x":1.}', "syntax"),
    (b'{"translation":"ok","x":.5}', "syntax"),
    (b'{"translation":"ok","x":+1}', "syntax"),
    (b'{"translation":"ok","x":1e}', "syntax"),
    (b'{"translation":"\\U0041"}', "syntax"),
    (b'{"translation":NaN}', "syntax"),
    (b'{"translation":"ok","x":Infinity}', "syntax"),
    (b'\xef\xbb\xbf{"translation":"ok"}', "syntax"),
    (b'', "syntax"),
    (b' \t\r\n', "syntax"),
    (b'{"translation":"ok"}{}', "syntax"),
    (b'{"translation":"\xff"}', "utf8"),
    (b'{"translation":"\xed\xa0\x80"}', "utf8"),            # encoded surrogate
    (b'{"translation":"a\\u0000b"}', "nul"),
    (b'{"translation":"a\\u00000"}', "nul"),
    (b'{"translation\\u0000junk":"WRONG","translation":"right"}', "nul"),
    (b'{"translation":"ok","x":"\\u0000","x":"safe"}', "nul"),               # duplicates are validated too
    (b'{"translation":"ok","x":{"k":"\\u0000"},"x":0}', "nul"),
    (b'{"translation":"\\ud800"}', "surrogate"),
    (b'{"translation":"\\udc00"}', "surrogate"),
    (b'{"translation":"\\ud800x\\udc00"}', "surrogate"),
    (b'{"translation":"\\ud800\\u0041"}', "surrogate"),
    (b'{"translation":"\\ud800\\u0000"}', "surrogate"),
    (b'{"translation":"\\ud800\\uZZZZ"}', "surrogate"),   # high surrogate is the first violation
    (b'{"translation":"\\ud800\\u', "surrogate"),
    (b'{"translation":"\\ud800\\q"}', "surrogate"),
    (b'{"translation":"\\ud800', "surrogate"),
    (b'{"translation":"\\x\\ud800"}', "syntax"),           # invalid escape comes first
    (b'{"translation":"\x01\\u0000"}', "syntax"),          # raw control byte comes first
    (b'{"translation":"\\u0000\x01"}', "nul"),
    (b'{"translation":"ok","x":"\\ud800","x":"safe"}', "surrogate"),
    (b'{"translation":"ok","x":' + b'[' * 40 + b']' * 40 + b',"x":0}', "depth"),
    (b'["translation"]', "root"),
    (b'"ok"', "root"),
    (b'42', "root"),
    (b'null', "root"),
    (b'{"translation":"a","translation":"b"}', "duplicate"),
    (b'{"translation":"ok","mixed":true,"mixed":false}', "duplicate"),
    (b'{"x":{"translation":"a","translation":"b"},"translation":"ok"}', "accept"),  # only root keys are reserved
    (b'{}', "translation"),
    (b'[]', "root"),
    (b'{"translation":1}', "translation"),
    (b'{"translation":null}', "translation"),
    (b'{"translation":true}', "translation"),
]


def run_cases():
    failures = 0
    for data, expected in CASES:
        try:
            validate(data)
            got = "accept"
        except Reject as exc:
            got = str(exc)
        if got != expected:
            failures += 1
            print(f"BAD expected={expected} got={got} input={data[:60]!r}")
    print(f"strict JSON reference: {len(CASES)} cases, {failures} failing")
    return failures


def _normalize(value):
    """Our parse result in the shape json.loads produces (last duplicate wins)."""
    import json
    if isinstance(value, Obj):
        return {k: _normalize(v) for k, v in value}
    if isinstance(value, list):
        return [_normalize(v) for v in value]
    if isinstance(value, Number):
        return json.loads(value)            # short lexemes only (see fuzz bounds)
    return value


def fuzz(seed=20261106, samples=200000):
    """1. Syntax: plain RFC 8259 (extra rules off) against Python's json module on
       small mutated documents (<= a few hundred bytes, so json's integer and
       recursion limits cannot trigger), plus decoded-value equality when both
       accept. This is bounded evidence, not proof of unrestricted equivalence.
    2. Coverage: the full validator's reason distribution on the same inputs."""
    import collections
    import json
    seeds = [c for c, _ in CASES if len(c) < 200] + [
        b'{"a":[1,2,{"b":null}],"c":"\\n\\u00e9"}', b'[true,false,-1.5e-3,"x"]', b'{"k":{"k":{"k":[]}}}',
        b'"\\ud800\\ud800\\udc00"', b'{"a":"\\ud83d\\ude00\\ud800x"}']
    alphabet = b' \t\n\r{}[]:,"\\/-+.0123456789eEtrufalsn\x00\x01\x7fu\xc3\xa9dDcC8'
    rnd = random.Random(seed)
    syntax_mismatch = value_mismatch = both_accept = 0
    reasons = collections.Counter()
    for _ in range(samples):
        b = bytearray(rnd.choice(seeds))
        for _ in range(rnd.randrange(1, 4)):
            op, pos = rnd.randrange(3), rnd.randrange(len(b) + 1)
            if op == 0 and b:
                del b[min(pos, len(b) - 1)]
            elif op == 1:
                b.insert(pos, rnd.choice(alphabet))
            elif b:
                b[min(pos, len(b) - 1)] = rnd.choice(alphabet)
        data = bytes(b)
        try:
            ours, ok_ours = parse(data, extras=False), True
        except Reject:
            ours, ok_ours = None, False
        try:
            def no_constants(_): raise ValueError
            theirs, ok_theirs = json.loads(data.decode("utf-8"), strict=True, parse_constant=no_constants), True
        except (UnicodeDecodeError, ValueError, RecursionError):
            theirs, ok_theirs = None, False
        if ok_ours != ok_theirs:
            syntax_mismatch += 1
            if syntax_mismatch <= 5:
                print(f"SYNTAX MISMATCH ours={ok_ours} json={ok_theirs} input={data!r}")
        elif ok_ours:
            both_accept += 1
            if _normalize(ours) != theirs:
                value_mismatch += 1
                if value_mismatch <= 5:
                    print(f"VALUE MISMATCH input={data!r} ours={_normalize(ours)!r} json={theirs!r}")
        try:
            validate(data)
            reasons["accept"] += 1
        except Reject as exc:
            reasons[str(exc)] += 1
    print(f"fuzz syntax: {samples} mutated documents (seed {seed}), syntax disagreements {syntax_mismatch}, "
          f"both accepted {both_accept}, decoded-value disagreements {value_mismatch}")
    print("fuzz full-validator reasons:", dict(sorted(reasons.items())))
    return syntax_mismatch + value_mismatch


if __name__ == "__main__":
    bad = run_cases()
    if sys.argv[1:2] == ["--fuzz"]:
        bad += fuzz()
    sys.exit(1 if bad else 0)
