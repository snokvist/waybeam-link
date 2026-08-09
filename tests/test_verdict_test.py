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
carried the same defect since it was written. Two files out of sixty-three, and
nothing in the build could see either — the compiler is happy, the linker is
happy, and ctest reports a pass.

The rule is deliberately about the EXIT PATH, not about the assertions: a suite
may legitimately have few CHECKs, but a suite that cannot report a failure is
not a test. Same shape as node_layering_test.py — source-level, no build
artifact, and it carries a self-test because a matcher that silently stops
matching is exactly the failure it exists to prevent.
"""
import re
import sys
from pathlib import Path

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent

# The harness whose verdict is separate from its assertions.
INCLUDES_HARNESS = re.compile(r'#\s*include\s*"wbtest\.h"')
# A real call, not the word in a comment or a mention in a string. Requiring the
# open paren is what distinguishes `return wbtest_finish("x");` from a doc line
# that merely names it.
CALLS_FINISH = re.compile(r'\bwbtest_finish\s*\(')


def strip_comments(text):
    """Remove // and /* */ so a mention in prose cannot satisfy the rule.

    Not a C++ parser and does not need to be: it only has to stop a comment
    from counting as a call. String literals are left alone — a literal
    containing `wbtest_finish(` would be a false negative, which is the safe
    direction (it can only make the gate stricter than reality, never laxer).
    """
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def offenders(files):
    bad = []
    for path in files:
        text = path.read_text()
        if not INCLUDES_HARNESS.search(strip_comments(text)):
            continue
        if not CALLS_FINISH.search(strip_comments(text)):
            bad.append(path)
    return bad


# (source, is_offender). Every entry is a case that actually came up or that a
# plausible edit would produce.
_SELF_TEST = [
    ('#include "wbtest.h"\nint main() { return 0; }\n', True),
    ('#include "wbtest.h"\nint main() { return wbtest_finish("x"); }\n', False),
    # The bug's exact shape: a summary printed by hand, then a bare return.
    ('#include "wbtest.h"\nint main() {\n  CHECK(1);\n'
     '  std::printf("ok\\n");\n  return 0;\n}\n', True),
    # A comment or doc mention must not satisfy it.
    ('#include "wbtest.h"\n// remember to call wbtest_finish() here\n'
     'int main() { return 0; }\n', True),
    ('#include "wbtest.h"\n/* wbtest_finish(...) */\nint main(){return 0;}\n', True),
    # The name without a call is not a call.
    ('#include "wbtest.h"\nint main(){ auto f = wbtest_finish; return 0; }\n', True),
    # A file that does not use the harness is out of scope, however it exits.
    ('#include <cstdio>\nint main() { return 0; }\n', False),
    ('// #include "wbtest.h"\nint main() { return 0; }\n', False),
    # Whitespace variants of both the include and the call.
    ('#  include  "wbtest.h"\nint main(){ return wbtest_finish ("x"); }\n', False),
]


def self_test(tmp):
    bad = []
    for i, (src, want) in enumerate(_SELF_TEST):
        p = tmp / f"case_{i}.cpp"
        p.write_text(src)
        got = bool(offenders([p]))
        if got != want:
            bad.append(f"self-test case {i}: expected offender={want}, got "
                       f"{got}, for:\n    {src.splitlines()[0]!r}")
        p.unlink()
    return bad


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        failures = self_test(Path(td))

    files = sorted(TESTS.glob("*_test.cpp")) + sorted(TESTS.glob("*_test.c"))
    if not files:
        # Cannot pass vacuously: an empty scan means the glob broke, not that
        # every suite is well-formed.
        failures.append("found no *_test.cpp under tests/ — the scan is broken")

    for path in offenders(files):
        failures.append(
            f"{path.relative_to(ROOT)}: includes wbtest.h but never calls "
            f"wbtest_finish(). CHECK only counts failures — without it this "
            f"suite runs every assertion, prints every failure and still exits "
            f"0, so it CANNOT FAIL. End main() with "
            f"`return wbtest_finish(\"{path.stem}\");`")

    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    if failures:
        return 1
    print(f"test_verdict_test: {len(files)} suites, all report their verdict")
    return 0


if __name__ == "__main__":
    sys.exit(main())
