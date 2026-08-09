#!/usr/bin/env python3
"""Every wbtest.h suite must actually be able to FAIL.

`tests/wbtest.h` splits the assertion from the verdict: `CHECK` prints and
increments `wbtest::failures`, and `wbtest_finish()` is the ONLY thing that
turns that count into a process exit code. So a `main()` ending in `return 0`
runs every assertion, prints every failure to stderr, and reports SUCCESS to
CTest. The suite is green and proves nothing.

That is not hypothetical. `tests/tx_node_c_test.cpp` shipped this way in the
first draft of link PR #165 and pre-merge review caught it by breaking three
behaviours at once and watching ctest stay green; `tests/mcs_probe_test.cpp` had
carried the same defect since it was written. Nothing in the build could see
either — the compiler is happy, the linker is happy, and ctest reports a pass.

The rule is about the EXIT PATH, not about the assertions: a suite may
legitimately have few CHECKs, but a suite that cannot report a failure is not a
test. It requires `return wbtest_finish(...)`, not merely a call — review
demonstrated that `wbtest_finish("x"); return 0;` compiles, discards the
verdict, and still exits 0 with a failing CHECK. That is the shape a developer
produces when mechanically appeasing a gate, and every one of the in-scope files
already returns it, so the stricter form costs nothing.

SCOPE, stated because the gate must not overclaim: it covers files that include
`wbtest.h`. Three suites (`calibrate_test`, `calib_dwell_test`,
`uplink_calibrate_test`) hand-roll the same split-verdict shape with a private
counter and are correct today, but they are NOT checked here — so the summary
line reports the scanned count and names the remainder rather than implying
coverage it does not have. An earlier draft printed the globbed count, which
made this script commit the exact vacuous-pass sin it exists to police.

Same shape as node_layering_test.py — source-level, no build artifact, and it
carries a self-test because a matcher that silently stops matching is precisely
the failure it exists to prevent.
"""
import re
import sys
from pathlib import Path

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent

INCLUDES_HARNESS = re.compile(r'#\s*include\s*"wbtest\.h"')
# `return wbtest_finish(` — not merely a call. See the module docstring.
RETURNS_VERDICT = re.compile(r'\breturn\s+wbtest_finish\s*\(')


def strip_comments(text):
    """Remove comments without touching string literals.

    A regex cannot do this safely and review demonstrated both failure
    directions on the previous draft: a `//` line mentioning `/*` teamed with a
    later `*/` in a literal made a DOTALL block-comment match swallow the
    `#include "wbtest.h"` — dropping a genuinely broken file out of scope
    entirely — while a `//` inside a string like "http://x" deleted a real
    `return wbtest_finish(...)` on the same line.

    Raw strings are handled explicitly: three suites here use `R"({ ... })"`
    holding JSON, so a scanner that treated the inner quotes as delimiters
    would be worse than the regex it replaces.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        # Raw string: R"delim( ... )delim"
        if c == 'R' and i + 1 < n and text[i + 1] == '"':
            close = text.find('(', i + 2)
            if close != -1:
                delim = text[i + 2:close]
                end = text.find(')' + delim + '"', close)
                if end != -1:
                    end += len(delim) + 2
                    out.append(text[i:end])
                    i = end
                    continue
        if c in '"\'':
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                i += 1
                if text[i - 1] == quote:
                    break
            continue
        if c == '/' and i + 1 < n:
            if text[i + 1] == '/':
                while i < n and text[i] != '\n':
                    i += 1
                continue
            if text[i + 1] == '*':
                end = text.find('*/', i + 2)
                i = n if end == -1 else end + 2
                continue
        out.append(c)
        i += 1
    return ''.join(out)


def classify(files):
    """-> (in_scope, offenders, out_of_scope)."""
    in_scope, offenders, out_of_scope = [], [], []
    for path in files:
        code = strip_comments(path.read_text())
        if not INCLUDES_HARNESS.search(code):
            out_of_scope.append(path)
            continue
        in_scope.append(path)
        if not RETURNS_VERDICT.search(code):
            offenders.append(path)
    return in_scope, offenders, out_of_scope


# (source, is_offender). Every entry is a case that came up in review or that a
# plausible edit would produce. A case is an "offender" only if it is in scope.
_SELF_TEST = [
    ('#include "wbtest.h"\nint main() { return 0; }\n', True),
    ('#include "wbtest.h"\nint main() { return wbtest_finish("x"); }\n', False),
    # The bug's exact shape: a summary printed by hand, then a bare return.
    ('#include "wbtest.h"\nint main() {\n  CHECK(1);\n'
     '  std::printf("ok\\n");\n  return 0;\n}\n', True),
    # Called but DISCARDED — compiles, exits 0 with a failing CHECK.
    ('#include "wbtest.h"\nint main() { wbtest_finish("x"); return 0; }\n', True),
    # A comment or doc mention must not satisfy it.
    ('#include "wbtest.h"\n// remember to return wbtest_finish() here\n'
     'int main() { return 0; }\n', True),
    ('#include "wbtest.h"\n/* return wbtest_finish(...) */\nint main(){return 0;}\n', True),
    ('#include "wbtest.h"\nint main(){ auto f = wbtest_finish; return 0; }\n', True),
    # Review's defeat #1: a // line naming /* plus a later */ in a literal made
    # the old DOTALL match delete the include, taking a broken file out of scope.
    ('#include "wbtest.h"\n// see /* below\nconst char* s = "*/";\n'
     'int main() { return 0; }\n', True),
    # Review's defeat #2: // inside a string literal ate the real call.
    ('#include "wbtest.h"\nint main() { const char* u = "http://x";'
     ' return wbtest_finish("f"); }\n', False),
    # Raw strings holding JSON, as three real suites do.
    ('#include "wbtest.h"\nconst char* j = R"({"a": "//not a comment"})";\n'
     'int main() { return wbtest_finish("f"); }\n', False),
    ('#include "wbtest.h"\nconst char* j = R"({"a": 1})";\nint main(){ return 0; }\n', True),
    # Out of scope, however it exits.
    ('#include <cstdio>\nint main() { return 0; }\n', False),
    ('// #include "wbtest.h"\nint main() { return 0; }\n', False),
    # Whitespace variants of both the include and the return.
    ('#  include  "wbtest.h"\nint main(){ return  wbtest_finish ("x"); }\n', False),
]


def self_test(tmp):
    bad = []
    for i, (src, want) in enumerate(_SELF_TEST):
        p = tmp / f"case_{i}.cpp"
        p.write_text(src)
        _, offenders, _ = classify([p])
        got = bool(offenders)
        if got != want:
            bad.append(f"self-test case {i}: expected offender={want}, got "
                       f"{got}, for:\n    {src.splitlines()[0]!r}")
        p.unlink()
    return bad


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        failures = self_test(Path(td))

    files = sorted(TESTS.glob("*_test.cpp"))
    if not files:
        # Cannot pass vacuously: an empty scan means the glob broke, not that
        # every suite is well-formed.
        failures.append("found no *_test.cpp under tests/ — the scan is broken")

    in_scope, offenders, out_of_scope = classify(files)
    for path in offenders:
        failures.append(
            f"{path.relative_to(ROOT)}: includes wbtest.h but does not end in "
            f"`return wbtest_finish(...)`. CHECK only counts failures — without "
            f"returning it this suite runs every assertion, prints every "
            f"failure and still exits 0, so it CANNOT FAIL. Use "
            f"`return wbtest_finish(\"{path.stem}\");`")

    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    if failures:
        return 1
    # Report what was SCANNED, and name what was not. Printing the globbed count
    # here would claim coverage of the hand-rolled suites, which is the vacuous
    # pass this gate exists to prevent.
    print(f"test_verdict_test: {len(in_scope)} wbtest.h suites checked, all "
          f"return their verdict")
    if out_of_scope:
        print("  not covered (own harness): "
              + ", ".join(p.name for p in out_of_scope))
    return 0


if __name__ == "__main__":
    sys.exit(main())
