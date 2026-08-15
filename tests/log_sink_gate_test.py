#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Library diagnostics go through the #144 sink, never fprintf(stderr).

io/ and node/ are the library: an embedding consumer (waybeam-hub in-process,
Android's :wifi over JNI) has no stderr worth reading, so a diagnostic written
there vanishes exactly when it matters — the wrong-bus adapter, the refused
config, the wedging SHM buffer. io/src was converted under #144 and node/
under the R6 log-sink item; this gate is what keeps both converted, because
`fprintf(stderr, ...)` is the reflex the next hundred diagnostics will reach
for.

Scope: every source and header under io/ and node/. app/ is the daemon and
stderr is its birthright; tests/ and tools/ likewise. io/src/log.cpp is the
one legitimate stderr writer — it IS the default sink — and stays visible to
this gate by never spelling `fprintf(stderr` (it uses fwrite on the sink
path), so it needs no exemption list entry. An exemption list deliberately
does not exist: the first entry would grow.

Matches are code, not prose — the conversion left comments that name the
banned call while explaining the rule, and a gate that flags its own
documentation teaches people to stop writing it down.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCOPES = [ROOT / "io", ROOT / "node"]

# `std::fprintf(stderr` and bare `fprintf(stderr`, with any spacing. The
# stream argument is what convicts: fprintf to a real file (the packet trace,
# the aim histogram dump target) is data, not diagnostics, and stays legal.
BANNED = re.compile(r"\bfprintf\s*\(\s*stderr\b")

# Line comments and block-comment bodies, same shape node_layering_test.py
# uses: the rule is about code, and the docs name the call they ban.
COMMENT = re.compile(r"^\s*(//|\*|/\*)")


def sources():
    out = []
    for scope in SCOPES:
        if not scope.exists():
            continue
        out.extend(
            p for p in sorted(scope.rglob("*"))
            if p.suffix in (".h", ".hpp", ".cpp", ".cc"))
    return out


def offenders(text):
    hits = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if COMMENT.match(line):
            continue
        if BANNED.search(line):
            hits.append((lineno, line.strip()))
    return hits


def self_test():
    """The matcher's contract, asserted on every run — a guard that silently
    stops matching is worse than no guard (node_layering_test.py, rule 3)."""
    must_catch = [
        'std::fprintf(stderr, "x\\n");',
        'fprintf(stderr, "x\\n");',
        'fprintf( stderr , "x");',
        'if (bad) std::fprintf(stderr,',
    ]
    must_pass = [
        '// diagnostics go through wb_logf, not fprintf(stderr)',
        ' * fprintf(stderr) is banned here',
        'std::fprintf(trace_file, "x\\n");',
        'wb_logf("x\\n");',
        'std::fwrite(msg, 1, n, stderr);',
    ]
    for s in must_catch:
        if not offenders(s):
            print(f"SELF-TEST FAIL: matcher missed: {s}")
            return False
    for s in must_pass:
        if offenders(s):
            print(f"SELF-TEST FAIL: matcher wrongly flagged: {s}")
            return False
    return True


def main():
    if not self_test():
        return 1
    files = sources()
    if len(files) < 50:
        # The scope walk itself is load-bearing: an empty or misrooted scan
        # passes every rule while checking nothing.
        print(f"FAIL: only {len(files)} sources under io/ + node/ — "
              "scope walk is broken")
        return 1
    bad = 0
    for path in files:
        for lineno, line in offenders(path.read_text(errors="replace")):
            print(f"{path.relative_to(ROOT)}:{lineno}: {line}")
            bad += 1
    if bad:
        print(f"FAIL: {bad} fprintf(stderr) site(s) in library code — "
              "route them through wb_logf (io/include/wblink/log.h, #144)")
        return 1
    print(f"log_sink_gate: {len(files)} sources clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
