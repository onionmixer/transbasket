"""V1 of PLAN_BACKEND_MODULES.md: was step 1 a pure move?

    python3 -I tests/check_move.py <old http_client.c> <new .c files...>

Both sides are cut into top-level ITEMS: a function definition, a typedef, a static
data definition, the dispatch table, one #include or #define line - each with the
comment lines directly above it (no blank line between). A comment separated by a
blank line is an item of its own. Braces and parentheses are counted outside
comments and string/char literals only, so preprocessor blocks and blank lines
inside a function stay part of that function.

(a) every old item must appear verbatim in exactly one new file - except the six
    functions whose definition line lost its leading `static ` (and nothing else),
    and the two split functions listed in SPLIT, which are printed for review;
(b) every new item must be an old item, an #include line, a new file's header
    comment, or one of the NEW items, which are printed for review.
Exit 0 only when (a) and (b) hold.
"""
import difflib
import sys
from pathlib import Path

UNSTATIC = {"str_replace", "build_instruction_message", "open_trace_file", "trace_base",
            "translate_via_openai", "translate_via_codex"}
SPLIT = {"openai_translator_init", "openai_translator_free"}
NEW = {"normal_backend_init", "codex_backend_init", "codex_backend_free",
       "openai_translator_init", "openai_translator_free"}


def scan(line, state):
    """Update state (depth, in_comment) over one line; return True if the line has code
    outside comments."""
    code = False
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if state["in_comment"]:
            if line.startswith("*/", i):
                state["in_comment"] = False
                i += 2
                continue
            i += 1
            continue
        if line.startswith("/*", i):
            state["in_comment"] = True
            i += 2
            continue
        if line.startswith("//", i):
            break
        if c in "\"'":
            code = True
            q, i = c, i + 1
            while i < n and line[i] != q:
                i += 2 if line[i] == "\\" else 1
            i += 1
            continue
        if not c.isspace():
            code = True
        if c in "{(":
            state["depth"] += 1
        elif c in "})":
            state["depth"] -= 1
        i += 1
    return code


def code_tail(line):
    """The line without a trailing // or /* */ comment, stripped (good enough to test ; and })."""
    for mark in ("//", "/*"):
        k = line.find(mark)
        if k >= 0 and line[:k].count('"') % 2 == 0:
            line = line[:k]
    return line.rstrip()


def items(text):
    state = {"depth": 0, "in_comment": False}
    out, cur = [], []
    for line in text.split("\n"):
        if state["depth"] == 0 and not state["in_comment"] and not line.strip():
            if cur:
                out.append("\n".join(cur))
                cur = []
            continue
        cur.append(line)
        has_code = scan(line, state)
        if state["depth"] == 0 and not state["in_comment"] and has_code:
            tail = code_tail(line)
            if line.lstrip().startswith("#") or tail.endswith(";") or tail.endswith("}"):
                out.append("\n".join(cur))
                cur = []
    if cur:
        out.append("\n".join(cur))
    assert state["depth"] == 0 and not state["in_comment"], state
    return out


def name_of(item):
    """Name of the function an item defines, or None."""
    for line in item.split("\n"):
        s = line.strip()
        if not s or s.startswith(("/*", "*", "//", "#")):
            continue
        if "(" in s and not s.startswith(("typedef", "return")):
            head = s.split("(")[0].replace("*", " ").split()
            return head[-1] if head else None
        return None
    return None


def unstatic(item):
    lines = item.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("static ") and "(" in line:
            lines[i] = line[len("static "):]
            return "\n".join(lines)
    return item


def main():
    old_path, new_paths = sys.argv[1], sys.argv[2:]
    old = items(Path(old_path).read_text(encoding="utf-8"))
    new = {p: items(Path(p).read_text(encoding="utf-8")) for p in new_paths}
    fail = []

    # (a) old -> new
    moved = 0
    for it in old:
        if it.startswith("#include"):
            continue                      # includes are compared as a set below
        nm = name_of(it)
        want = unstatic(it) if nm in UNSTATIC else it
        where = [p for p, its in new.items() for x in its if x == want]
        if nm in SPLIT:
            continue
        if len(where) != 1:
            fail.append(f"(a) old item {nm or it.splitlines()[0][:60]!r} found {len(where)} times")
        else:
            moved += 1
    old_includes = {it for it in old if it.startswith("#include")}
    # every new file must keep the old include block (plan section 3: same declarations everywhere)
    for p, its in new.items():
        missing = old_includes - {x for x in its if x.startswith("#include")}
        if missing:
            fail.append(f"(a) {p} lacks {sorted(missing)}")

    # (b) new -> old
    old_set = set(old) | {unstatic(it) for it in old if name_of(it) in UNSTATIC}
    review = []
    for p, its in new.items():
        for k, it in enumerate(its):
            if it in old_set or it.startswith("#include"):
                continue
            if k == 0 and it.startswith("/**") and "Split out of http_client.c" in it:
                continue
            nm = name_of(it)
            if nm in NEW:
                review.append((p, nm, it))
                continue
            fail.append(f"(b) {p}: new item not in the old file: {it.splitlines()[0][:70]!r}")

    print(f"old items: {len(old)} ({len(old_includes)} includes), moved verbatim: {moved}, "
          f"unstatic: {len(UNSTATIC)}, split: {len(SPLIT)}")
    print(f"new files: {len(new)}, new items for review: {len(review)}")
    for p, nm, it in review:
        print(f"\n---- review: {nm} ({p})")
        olds = [o for o in old if name_of(o) == nm]
        if olds:
            for l in difflib.unified_diff(olds[0].split("\n"), it.split("\n"), "old", "new", lineterm="", n=1):
                print("   " + l)
        else:
            print("   " + "\n   ".join(it.split("\n")))
    for f in fail:
        print("FAIL", f)
    print("check_move:", "OK" if not fail else f"{len(fail)} failure(s)")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
