#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The node/ layer owns no process, and nothing below it depends on it.

This is the standing answer to blocker B9 of issue #109 ("the library must not
own the process"). The B9 survey listed three process-level behaviours that are
correct for a daemon and wrong for a library: signal handlers installed without
SA_RESTART, `spawn_mode_applier`'s double fork, and the Pass 148 sustained-wedge
exit.

Phase 2a did not have to *design* an answer for them. All three stayed in
`app/main.cpp` — the driver — while every node-behaviour object moved to
`node/`, and the wedge path was already a `return kExitTxWedged` that `main()`
propagates rather than an `exit()` in the loop. So the answer is structural: a
consumer that links `wblink::node` never calls `main()` and therefore inherits
none of the three.

That property is worth exactly as much as it is enforced, which is why this
file exists. It reads sources directly and needs no build artifact.

Three rules:

  1. Nothing under node/ calls a process-owning primitive.
  2. Nothing under core/ or io/ includes a node/ header. The dependency runs
     one way: node/ may use core/ and io/, neither may use node/.
  3. No node/ HEADER defines a namespace-scope function without `inline`.

Rule 3 was added in Phase 2c step 2, after it fired for real. Phase 2a moved
eight free functions into node/ headers without `inline` — `emit_stats`,
`rx_policy`, `packet_type_name`, `bw_code`, `s_to_ms`, `selector_policy`,
`scheduler_policy`, `calib_params_from`. Every one is an ODR violation, and
none of them could be caught: `app/main.cpp` was the only translation unit
that included them, and one definition in one TU links fine. The moment the
RX loop became a second TU in the same binary, the linker rejected
`emit_stats` outright.

That is the same failure Phase 2a found with the aim histograms (file-`static`
is identical to `inline` in one TU and not otherwise), arriving a second time
by a different route — which is the argument for checking it mechanically
rather than remembering it.

Rule 3 checks its own matcher (`_SELF_TEST` below), because the first draft of
it was defeated by a trailing `//` comment — it stripped comments before the
regex but not before the brace test, so `... ) {  // note` passed the gate and
failed the link. A guard that silently stops matching is worse than no guard,
so the shapes it must catch and must not catch are asserted on every run.

It covers variables as well as functions. That is not symmetry for its own
sake: a namespace-scope `static` variable in a header never produces a link
error at all — every TU quietly gets its own copy, which is the aim-histogram
failure above, and strictly worse than a duplicate symbol.

Rule 3's limits, stated rather than discovered. It anchors at column 0, so a
tree formatted with `NamespaceIndentation: All` would disable it — nothing here
indents inside `namespace wblink {`, and no `.clang-format` pins that. It does
not know that an explicit template specialisation is not implicitly inline. The
linker remains the backstop in every case: this exists to fail in CI instead of
in whichever consumer adds the second TU.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NODE = ROOT / "node"
LOWER = [ROOT / "core", ROOT / "io"]

# Free-standing calls only. `calibrator_->abort(now)` and `x.exit_code()` are
# method calls on our own objects and are not process control, so the pattern
# requires the identifier to start a term rather than follow `.` or `->`.
FORBIDDEN = re.compile(
    r"(?<![\w.>])(?:_exit|exit|abort|quick_exit|fork|vfork|execl|execlp|execv|"
    r"execvp|signal|sigaction|raise|atexit|setjmp|longjmp)\s*\(")

# Line comments and block-comment bodies: the rules are about code, and the
# headers explain the rules in prose that names the very functions they ban.
COMMENT = re.compile(r"^\s*(//|\*|/\*)")


def sources(root, suffixes=(".h", ".hpp", ".cpp", ".cc")):
    if not root.exists():
        return []
    return sorted(p for p in root.rglob("*") if p.suffix in suffixes)


# Rule 3. A namespace-scope DEFINITION in a header that the linker will see
# twice. Two shapes, because the failure has two:
#
#   function without `inline`  -> "multiple definition", loud, caught at link
#   variable without `inline`  -> same, UNLESS it is `static`, in which case
#                                 every TU silently gets its own copy
#
# The second is the shape Phase 2a hit with the aim histograms, and it is the
# worse one: nothing fails, the copies just diverge. So both are checked.
#
# Definitions are found by joining a candidate's continuation lines until the
# signature terminates, then asking how it terminated: `{` opens a body and is
# a definition, `;` is a declaration and is fine. Doing it on the joined text
# rather than per line is what keeps a wrapped DECLARATION from being flagged.
_STRIP_COMMENT = re.compile(r"//.*$")
# Leading tokens that make a namespace-scope definition safe, or that mean the
# line is not one at all. `static` is NOT here: for a variable it is the silent
# failure above, so it is handled separately below.
_SAFE_PREFIX = re.compile(
    r"^(inline|template|constexpr|consteval|constinit|struct|class|enum|union|"
    r"namespace|using|extern|typedef|friend|return|public|private|protected)\b")
_ATTRIBUTE = re.compile(r"^(\[\[[^\]]*\]\]|__attribute__\s*\(\(.*?\)\))\s*")
_DECL_START = re.compile(r"^[A-Za-z_~\[]")


def odr_hazards(text, where):
    """Namespace-scope definitions in a header that lack `inline`."""
    out = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i]
        i += 1
        if COMMENT.match(raw) or not _DECL_START.match(raw):
            continue                      # indented, blank, or a preprocessor line
        stripped = _ATTRIBUTE.sub("", _STRIP_COMMENT.sub("", raw)).strip()
        if not stripped or _SAFE_PREFIX.match(stripped):
            continue
        start, joined, n = i, stripped, i
        # Join until the signature terminates. Bail on a blank line so a stray
        # non-declaration cannot swallow the rest of the file.
        while not joined.endswith(("{", ";", "}")) and n < len(lines):
            nxt = _STRIP_COMMENT.sub("", lines[n]).strip()
            n += 1
            if not nxt:
                break
            joined += " " + nxt
        if not joined.endswith("{"):
            continue                      # a declaration, or something else
        i = max(i, n)
        is_static = joined.startswith("static ")
        kind = ("function" if "(" in joined.split("{", 1)[0] else "variable")
        why = ("every TU gets its OWN copy, silently — no link error to catch it"
               if is_static else
               "one definition in one TU links, two do not")
        out.append(f"{where}:{start}: namespace-scope {kind} definition in a "
                   f"header must be `inline` — {why}\n    {stripped}")
    return out


# Rule 3 checks itself. The gate went in as part of the change that proved the
# bug was real, so "it worked when I tried it" is not good enough — a trailing
# `//` comment defeated the first draft, which is the cheapest possible way to
# reintroduce exactly what it exists to catch.
_SELF_TEST = [
    # (source, expected number of hazards)
    ("uint8_t bw_code(uint8_t w) {\n    return 0;\n}\n", 1),
    ("uint8_t bw_code(uint8_t w) {  // trailing comment\n}\n", 1),
    ("inline uint8_t bw_code(uint8_t w) {\n}\n", 0),
    ("[[nodiscard]] uint8_t bw_code(uint8_t w) {\n}\n", 1),
    ("void emit_stats(int a,\n                int b) {\n}\n", 1),   # wrapped definition
    ("int run_rx(const Loaded& l,\n           const std::atomic<int>& stop);\n", 0),
    ("inline int g = 0;\n", 0),
    ("static int g_counter = 0;\n", 0),          # a declaration-with-initialiser, not a body
    ("    uint8_t member(uint8_t w) {\n    }\n", 0),   # indented: a class member
    ("struct S {\n    int f() { return 0; }\n};\n", 0),
    ("// uint8_t bw_code(uint8_t w) {\n", 0),
    ("constexpr int k = 1;\n", 0),
]


def self_test():
    bad = []
    for src, want in _SELF_TEST:
        got = len(odr_hazards(src, "<self-test>"))
        if got != want:
            bad.append(f"rule 3 self-test: expected {want} hazard(s), got "
                       f"{got}, for:\n    {src.splitlines()[0]!r}")
    return bad



def main():
    failures = self_test()

    for path in sources(NODE):
        for n, line in enumerate(path.read_text().splitlines(), 1):
            if COMMENT.match(line):
                continue
            code = line.split("//", 1)[0]
            m = FORBIDDEN.search(code)
            if m:
                failures.append(
                    f"{path.relative_to(ROOT)}:{n}: node/ must not own the "
                    f"process (B9) — found {m.group(0).strip()}\n    {line.strip()}")

    for path in sources(NODE, (".h", ".hpp")):
        failures.extend(odr_hazards(path.read_text(), path.relative_to(ROOT)))

    inc = re.compile(r'#\s*include\s*[<"]wblink/node/')
    for root in LOWER:
        for path in sources(root):
            for n, line in enumerate(path.read_text().splitlines(), 1):
                if inc.search(line):
                    failures.append(
                        f"{path.relative_to(ROOT)}:{n}: layering inverted — "
                        f"{root.name}/ must not include a node/ header\n"
                        f"    {line.strip()}")

    checked = len(sources(NODE)) + sum(len(sources(r)) for r in LOWER)
    if not NODE.exists() or not sources(NODE):
        print("node_layering_test: FAIL — node/ has no sources; this guard "
              "would pass vacuously", file=sys.stderr)
        return 1

    for f in failures:
        print("node_layering_test: " + f, file=sys.stderr)
    print(f"node_layering_test: {checked} files checked, "
          f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
