"""Evaluate the v1 source_hint reference (tests/source_hint_ref.py).

    python3 -I tests/source_hint_eval.py [cache.db ...]
    python3 -I tests/source_hint_eval.py --decoder

Cache databases are opened read-only and only aggregate counts are printed,
never source text. Rows with an explicit `from` serve as labels; `auto` rows
have no label and are skipped. Without arguments only the fixed cases run.
"""
import collections
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import source_hint_ref as R  # noqa: E402

# (expected fallback lang, expected mixed, text) - expectations of the v1 rules,
# including documented limitations (marked LIMIT).
FALLBACK_CASES = [
    ("kor", False, "API 서버의 timeout 설정을 변경하고 retry policy를 확인하세요"),
    ("kor", True, "다음 명령을 실행하세요: docker compose up -d --build --remove-orphans"),
    ("jpn", False, "明日の会議は午後三時に始まります。"),
    ("chi", False, "東京大学医学部附属病院"),                       # LIMIT: kanji-only Japanese
    ("chi", False, "明天的会议下午三点开始。"),
    ("jpn", True, "회의 일정 공유드립니다. Meeting starts at 3pm. 会議は午後三時からです。"),  # LIMIT: model said kor
    ("kor", True, "OK 버튼"),                                        # 2 Hangul units vs 1 Latin word
    ("und", True, "OK버"),                                         # 1 Latin word vs 1 Hangul unit: tie -> und
    ("und", False, "2026-11-05 14:30 | 12345 | v1.2.3"),             # no letters
    ("und", False, "Le serveur redémarre automatiquement à minuit."),  # Latin is shared
    ("und", False, "Le serveur redémarre"),                    # decomposed accent stays one word
    ("und", False, ""),
    ("und", False, "\u037e\u0589\u0e50\u30fb\u05c3\u060c\u0660\u0964"),  # script punctuation/digits are Ignore
    ("und", False, "aªb"),                                          # ª is a Latin letter: one word
    ("jpn", False, "ラーメン"),                                      # prolonged sound mark is Kana
    ("tha", False, "ภาษาไทย"),                                      # Thai vowel signs (Mn) count as Thai
]

# (model lang, expected contradicted, text) - covers PLAN section 5.3 and codex
# review items 27 (unmodelled scripts), 28 (single Kana) and 29 (Hebrew/Yiddish).
OVERRIDE_CASES = [
    ("jpn", True, "안녕하세요 반갑습니다"),                           # Hangul-only, model says jpn
    ("eng", True, "안녕하세요 반갑습니다"),
    ("kor", False, "안녕하세요 반갑습니다"),
    ("kor", True, "Hello there, nice to meet you"),                  # kor needs Hangul or Han
    ("chi", False, "这是一个很长的中文句子用于测试的" * 6 + "の"),      # 99%+ Han + one Kana: no Japanese dominance
    ("ben", False, "আমি বাংলায় কথা বলি এবং লিখি " * 3 + "한"),        # Bengali is Other; one Hangul is not dominant
    ("yid", False, "איך רעד יידיש"),                                  # Hebrew script allows yid
    ("heb", False, "אני מדבר עברית"),
    ("eng", True, "אני מדבר עברית"),
    ("und", True, "안녕하세요 반갑습니다"),                           # und while text is clearly Korean
    ("und", False, "2026-11-05 | 12345"),
    ("fre", False, "Le serveur redémarre"),                          # Latin: no evidence either way
]


def run_cases():
    failures = 0
    for lang, mixed, text in FALLBACK_CASES:
        got = R.classify(text.encode("utf-8"))
        ok = got == (lang, mixed)
        failures += not ok
        print(f"  fallback {'ok ' if ok else 'BAD'} expected={(lang, mixed)} got={got}")
    for lang, expected, text in OVERRIDE_CASES:
        got = R.model_contradicted(text.encode("utf-8"), lang)
        ok = got == expected
        failures += not ok
        print(f"  override {'ok ' if ok else 'BAD'} model={lang} contradicted expected={expected} got={got}")
    return failures


def evaluate(paths):
    tally = collections.Counter()
    second_shares = []
    for path in paths:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        for from_lang, text in con.execute("SELECT from_lang, source_text FROM trans_cache"):
            if from_lang == "auto":
                tally["auto (unlabelled, skipped)"] += 1
                continue
            data = text.encode("utf-8")
            lang, _ = R.classify(data)
            tally["correct" if lang == from_lang else "abstain(und)" if lang == "und"
                  else f"WRONG {from_lang}->{lang}"] += 1
            g = R._merged(R.count_units(data), lambda kana, han: True)
            if len(g) >= 2:
                values = sorted(g.values(), reverse=True)
                second_shares.append((values[1], sum(values)))
        con.close()
    print(f"labelled rows: {sum(v for k, v in tally.items() if not k.startswith('auto'))}", dict(tally))
    second_shares.sort(key=lambda st: st[0] * 10**9 // st[1])
    shown = [round(second / total, 3) for second, total in second_shares]   # display only
    print(f"rows with >= 2 buckets: {len(second_shares)}; second-bucket shares: {shown}")
    for th in (20, 30, 40):
        hits = sum(1 for second, total in second_shares if 100 * second >= th * total)
        print(f"  mixed at >= {th}%: {hits} of {len(second_shares)}")


def decoder_selftest(seed=20261106, samples=200000):
    """Strict-decoder check: every scalar value round-trips, and random byte
    strings (length 1-6, biased towards lead/continuation bytes) agree with
    Python's strict codec on validity and decoded values."""
    import random
    roundtrip = sum(1 for cp in range(0x110000) if not 0xD800 <= cp <= 0xDFFF
                    and list(R.decode(chr(cp).encode("utf-8"))) != [cp])
    rnd = random.Random(seed)
    mismatch = 0
    for _ in range(samples):
        data = bytes(rnd.choice([rnd.randrange(256), rnd.randrange(0x80, 0x100),
                                 rnd.choice([0xE0, 0xED, 0xF0, 0xF4, 0xC0, 0xC1])])
                     for _ in range(rnd.randrange(1, 7)))
        ours = list(R.decode(data))
        try:
            ref, valid = [ord(c) for c in data.decode("utf-8")], True
        except UnicodeDecodeError:
            ref, valid = None, False
        if (R.INVALID in ours) == valid or (valid and ours != ref):
            mismatch += 1
    print(f"decoder: scalar round-trip failures {roundtrip}; random {samples} (seed {seed}) mismatches {mismatch}")
    return roundtrip + mismatch


if __name__ == "__main__":
    if sys.argv[1:2] == ["--decoder"]:
        sys.exit(1 if decoder_selftest() else 0)
    print(f"source_hint reference v{R.HINT_VERSION}")
    bad = run_cases()
    print(f"fixed cases: {len(FALLBACK_CASES) + len(OVERRIDE_CASES)} total, {bad} failing")
    if sys.argv[1:]:
        evaluate(sys.argv[1:])
    sys.exit(1 if bad else 0)
