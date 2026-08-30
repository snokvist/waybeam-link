#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The declared key registry matches the loader, in both directions.

io/src/config.cpp reads every key through value()/at()/contains(), so a key it
does not recognise is silently ignored (#106). io/src/config_registry.cpp is
the declared answer to "which keys exist"; this test keeps the two honest by
reconstructing the dotted path of every accessor site in the loader and
comparing the sets.

The reconstruction leans on one property of config.cpp: sub-objects are always
bound as `const json& v = parent.at("key")` and array elements as
`for (const json& v : parent.value("key", json::array()))`, so a variable's
dotted path is known before it is used. If that stops being true the binding
form is unrecognised, the accessor's object variable is unknown, and the site
lands in `skipped` — which is a hard failure here rather than a silent
undercount, because an undercount would let a real key go unregistered.

For the same reason the test also asserts that every accessor site in the whole
file falls inside a walked or explicitly exempt function: a key lookup added to
a new helper must show up as a failure, not as a key nobody checks.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOADER = os.path.join(ROOT, "io", "src", "config.cpp")
REGISTRY = os.path.join(ROOT, "io", "src", "config_registry.cpp")

# Regions of config.cpp that parse the node config, with the dotted path the
# region's `j` parameter refers to. parse_bind is reached from two parents
# (streams[].bind and stats.bind, config.cpp:828), so it is walked twice.
REGIONS = [
    ("load_config_json", ""),
    ("parse_bind", "streams[].bind"),
    ("parse_bind", "stats.bind"),
    # §15.2 (Pass 195): the power keys are parsed once for both adapter forms,
    # so like parse_bind this region is walked once per parent.
    ("parse_adapter_power", "adapters[]"),
    ("parse_adapter_power", "adapters.auto"),
]

# Sub-parsers reached from more than one parent. REGIONS hardcodes the parent
# path for each caller, so a caller nobody added leaves that whole sub-object
# unregistered while this test still reports agreement. The counts are pinned
# rather than trusted; see caller_count().
SHARED_PARSERS = ["parse_bind", "parse_adapter_power"]

# Functions that hold accessor calls which are NOT node-config keys. Listed so
# that "every accessor site in the file is accounted for" can be asserted:
# without that, a key lookup added to some new helper would be invisible here
# rather than reported, which is the one failure this test cannot afford.
#
# load_profile_table_json parses profiles.json — a separate artifact with its
# own schema (§9.3), not part of the node config surface.
EXEMPT = ["load_profile_table_json"]

BIND = re.compile(r'const json& (\w+)\s*=\s*(\w+)\.at\(\s*"([^"]+)"\s*\)', re.S)
LOOP = re.compile(r'for \(const json& (\w+) : (\w+)\.(?:value|at)\(\s*"([^"]+)"', re.S)
ACC = re.compile(r'(\w+)\.(?:value|at|contains)\(\s*"([^"]+)"', re.S)

# ACC only matches `identifier.value|at|contains("literal")`. These idioms read
# a key without matching it, and because unwalked_sites() uses the same regex
# they would be invisible rather than merely unresolved — the one outcome this
# test cannot afford. They are banned everywhere outside EXEMPT rather than
# modelled, because banning is checkable and inference is not.
FORBIDDEN = [
    (re.compile(r'\.(?:find|count|items)\('),
     'find()/count()/items() — enumerate or look up without a literal key'),
    (re.compile(r'\w\s*\[\s*"[^"]*"\s*\]'),
     'operator[]("key") — use .at()/.value() so the key is visible here'),
    (re.compile(r'\.(?:value|at|contains)\(\s*(?!")[A-Za-z_]'),
     'non-literal key argument — the key name must be a literal'),
    # `.at("k").get<T>()` is fine — it terminates. Only a chain into ANOTHER
    # key hides a level, because the reconstruction binds paths per variable.
    (re.compile(r'\.at\(\s*"[^"]*"\s*\)\s*\.(?:at|value|contains)\('),
     'chained .at("a").at("b") — bind the intermediate to a `const json&` '
     'first, or the reconstructed path silently loses a level'),
    # A lambda parameter named `j` shadows the seed, and the reconstruction
    # has no scopes: keys read off it resolve to top-level paths. That fails
    # loudly but with a WRONG remedy — the message says to register the
    # phantom path, and doing so goes green with a bogus key in the published
    # schema. Ban the shadow instead; any other parameter name is fine.
    (re.compile(r'\[[^\]\n]*\]\s*\([^)]*\bjson&\s*j\b'),
     'a lambda parameter named `j` shadows the seed object — name it '
     'anything else, or reconstructed paths silently lose their prefix'),
]


def function_span(text, name):
    """Byte range of a top-level function body, by brace matching."""
    m = re.search(r"^[\w:<>, ]+ " + re.escape(name) + r"\(", text, re.M)
    if m is None:
        sys.exit(f"FAIL: cannot find {name}() in {LOADER}")
    i = text.index("{", m.end() - 1)
    depth, j = 0, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return i, j
        j += 1
    sys.exit(f"FAIL: unbalanced braces in {name}()")


def blank_literals(text):
    """Blank comment and string-literal CONTENT, preserving length and lines.

    The FORBIDDEN patterns are regexes over C++ source, so a string literal
    that happens to contain `[`, `"` or `.at(` reads as code. config.cpp:584
    builds the error `"select.rung_rssi_floor_dbm[" + to_string(i) + "]"`,
    which a naive obj["key"] pattern matches straight through. Blanking the
    content (not the quotes) leaves `x[""]` still detectable while that error
    string becomes inert. Offsets are preserved so reported lines stay right.
    """
    out = list(text)

    def blank(lo, hi):
        for i in range(lo, hi):
            if out[i] != "\n":
                out[i] = " "

    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            blank(i + 1, j)  # keep both quotes
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return "".join(out)


def forbidden_sites(text):
    """Key-reading idioms ACC cannot see, anywhere outside EXEMPT.

    Scanning only the walked regions is not enough, and the gap is not
    theoretical: a helper that reads keys ONLY via operator[] or a non-literal
    key leaves no trace at all. ACC does not match it, so unwalked_sites() —
    which uses ACC — never sees the function either, and the whole helper is
    invisible rather than merely unresolved. Scanning everything except the
    explicitly exempt functions closes that.
    """
    code = blank_literals(text)
    exempt = [function_span(text, n) for n in EXEMPT]
    out = []
    for rx, why in FORBIDDEN:
        for m in rx.finditer(code):
            if any(lo <= m.start() < hi for lo, hi in exempt):
                continue
            out.append((text.count("\n", 0, m.start()) + 1,
                        " ".join(text[m.start():m.end()].split()), why))
    return sorted(set(out))


def caller_count(text, name):
    """How many places call `name`, so REGIONS cannot fall behind them.

    A shared sub-parser's parent path is hardcoded in REGIONS per caller. An
    unlisted caller would leave that whole sub-object unregistered while this
    test still reported agreement, so the count is pinned rather than trusted.
    One occurrence is the definition.

    Counted over blanked source: a comment that merely mentions parse_bind(
    would otherwise report a caller that does not exist, and the failure's
    prescribed remedy — adding a REGIONS entry — would then break the test a
    different way.
    """
    return len(re.findall(r'\b' + re.escape(name) + r'\(',
                          blank_literals(text))) - 1


def unwalked_sites(text):
    """Accessor sites outside every REGION and EXEMPT span.

    A key lookup that lands here is one this test would otherwise never see.
    """
    covered = [function_span(text, n) for n, _ in REGIONS]
    covered += [function_span(text, n) for n in EXEMPT]
    out = []
    for m in ACC.finditer(text):
        off = m.start()
        if any(lo <= off < hi for lo, hi in covered):
            continue
        out.append((text.count("\n", 0, off) + 1, m.group(1), m.group(2)))
    return out


def loader_paths():
    text = open(LOADER, encoding="utf-8").read()
    paths, skipped = set(), []
    for name, seed in REGIONS:
        lo, hi = function_span(text, name)
        sub = text[lo:hi]
        env = {"j": seed}
        events = []
        for m in BIND.finditer(sub):
            events.append((m.start(), "object", m))
        for m in LOOP.finditer(sub):
            events.append((m.start(), "array", m))
        for m in ACC.finditer(sub):
            events.append((m.start(), "acc", m))
        for off, kind, m in sorted(events, key=lambda e: e[0]):
            line = text.count("\n", 0, lo + off) + 1
            if kind == "acc":
                parent, key = m.groups()
                if parent not in env:
                    skipped.append((line, parent, key))
                    continue
                base = env[parent]
                paths.add(f"{base}.{key}" if base else key)
            else:
                var, parent, key = m.groups()
                if parent not in env:
                    skipped.append((line, parent, key))
                    continue
                base = env[parent]
                path = f"{base}.{key}" if base else key
                paths.add(path)
                env[var] = path + ("[]" if kind == "array" else "")
    return paths, skipped, unwalked_sites(text), forbidden_sites(text), text


def registry_paths():
    text = open(REGISTRY, encoding="utf-8").read()
    return set(re.findall(r'\{"([^"]+)",\s*KeyType::', text))


def main():
    loader, skipped, unwalked, forbidden, text = loader_paths()
    registry = registry_paths()
    fail = False

    for parser in SHARED_PARSERS:
        expected_callers = sum(1 for n, _ in REGIONS if n == parser)
        actual_callers = caller_count(text, parser)
        if actual_callers != expected_callers:
            fail = True
            print(f"FAIL: {parser} has {actual_callers} caller(s) but REGIONS "
                  f"seeds {expected_callers}.")
            print("      Add the new caller to REGIONS with the dotted path of "
                  "the object it parses, or")
            print("      that whole sub-object goes unregistered while this "
                  "test still reports agreement.")

    if forbidden:
        fail = True
        print(f"FAIL: {len(forbidden)} key-reading idiom(s) this test cannot "
              f"see:")
        for line, snippet, why in forbidden[:20]:
            print(f"        config.cpp:{line}: {snippet!r} — {why}")

    if unwalked:
        fail = True
        print(f"FAIL: {len(unwalked)} accessor site(s) in a function this test "
              f"does not walk.")
        print("      Add the function to REGIONS (with the dotted path its "
              "json parameter refers to)")
        print("      or to EXEMPT (if it parses something other than the node "
              "config).")
        for line, parent, key in unwalked[:20]:
            print(f'        config.cpp:{line}: {parent}.<"{key}">')

    if skipped:
        fail = True
        print(f"FAIL: {len(skipped)} accessor site(s) whose JSON object could "
              f"not be resolved to a path.")
        print("      A new binding form was introduced; teach REGIONS/BIND/LOOP "
              "above, do not ignore these —")
        print("      an unresolved site is a key this test is blind to.")
        for line, parent, key in skipped[:20]:
            print(f'        config.cpp:{line}: {parent}.<"{key}">')

    missing = sorted(loader - registry)
    if missing:
        fail = True
        print(f"FAIL: {len(missing)} key(s) parsed by the loader but not in "
              f"io/src/config_registry.cpp:")
        for p in missing:
            print(f"        {p}")
        print("      Add them, then rebuild — config-schema --json picks them up.")

    extra = sorted(registry - loader)
    if extra:
        fail = True
        print(f"FAIL: {len(extra)} key(s) registered but never read by the "
              f"loader (dead entry, or the loader dropped support):")
        for p in extra:
            print(f"        {p}")

    if fail:
        return 1
    print(f"config_registry: {len(loader)} keys, loader and registry agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
