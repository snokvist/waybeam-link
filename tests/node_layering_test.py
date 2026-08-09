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

Two rules:

  1. Nothing under node/ calls a process-owning primitive.
  2. Nothing under core/ or io/ includes a node/ header. The dependency runs
     one way: node/ may use core/ and io/, neither may use node/.
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


def main():
    failures = []

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
