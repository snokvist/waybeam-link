#!/usr/bin/env python3
"""Verify that an #109 extraction step is a MOVE and not a rewrite.

Issue #109 lifts code out of `app/main.cpp` into `node/`. The value of doing
it verbatim is that the change cannot alter behaviour — but "verbatim" is a
claim, and a green test suite does not check it: the suites exercise the new
location, so a block that picked up an inverted condition on the way out
passes them exactly as well as one that did not.

This checks the claim. For each moved block it takes the exact line range out
of `app/main.cpp` at a base revision, applies the small set of edits a header
requires, and asserts the result appears verbatim in the destination file.
Anything else — a changed constant, a dropped line, a helpfully reworded
comment — fails.

    tools/move_identity.py                    # every step, each against its
                                              # own recorded base
    tools/move_identity.py - "step 2"         # one step
    tools/move_identity.py HEAD~1 "step 2"    # override the base, for a step
                                              # that has not landed yet

BASE_REV is whatever `app/main.cpp` looked like before the step. It moves with
every rebase, which is why this is a review-time tool rather than something
`scripts/gates.sh` runs: after the step lands there is no fixed revision for
it to compare against.

What this does NOT prove, stated so it is not over-read: the destination
check is a substring test, so it cannot see code ADDED around the moved block,
and edits are applied to every occurrence, so it cannot tell one application
from forty. The per-block line here reports how many edits actually fired,
which is the number a reviewer should compare against the PR's claim; a diff
in both directions is still the stronger check when one is warranted.

Adding a step: append its blocks to STEPS with the source ranges as they were
at that step's base, and add any new permitted edit to EDITS. Keep the edit
list minimal — its whole purpose is that a mechanical transformation nobody
declared shows up as a failure.
"""
import subprocess
import sys

N = "node/include/wblink/node/"

# (label, first, last, destination) — 1-indexed inclusive, as the block was in
# app/main.cpp at BASE_REV.
# Each step records the base its ranges were measured against, so a step can
# be checked without remembering which commit that was — getting it wrong
# fails all blocks at once, which reads alarmingly like a real regression.
# BASE_REV on the command line overrides, for checking a step before it lands.
BASES = {
    "2c step 1 — run_rx's free-function dependencies": "109a704^",
    "2c step 2 — the RX run loop": "109a704",
    "Phase 3 prep — Loaded's loader": "7c25796",
}

# A step stops being checkable once its DESTINATION legitimately changes — the
# check compares moved text against a base, and later edits to the moved code
# are not regressions. Recording where each one last passed keeps the evidence
# without turning normal work into a red gate. Verify a historical step by
# checking out the commit named here and running the tool there.
#
# `run_rx` diverged when B10 (#160) added the FrameSink parameter, which is
# exactly the kind of edit this tool must NOT object to.
VERIFIED_AT = {
    "2c step 2 — the RX run loop": "7470417 (PR #159), before B10 changed the "
                                   "signature",
}

STEPS = {
    "2c step 1 — run_rx's free-function dependencies": [
        ("csa_params",             189, 206, N + "policy.h"),
        ("vcmd_params",            208, 221, N + "policy.h"),
        ("channel_allowed",        412, 415, N + "policy.h"),
        ("quietgap_policy",        481, 487, N + "policy.h"),
        ("vcmd_id_for",            223, 236, N + "vcmd.h"),
        ("vcmd_name_for",          381, 395, N + "vcmd.h"),
        ("mtu_tier_for_mode",      397, 408, N + "vcmd.h"),
        ("frame_is_eob",           417, 422, N + "frame_kind.h"),
        ("frame_is_paced_eob",     424, 434, N + "frame_kind.h"),
        ("frame_is_live_rtp_data", 436, 441, N + "frame_kind.h"),
        ("session_nonce",          443, 454, N + "entropy.h"),
        ("announce_token",         456, 473, N + "entropy.h"),
        ("power_tier_json",        239, 255, N + "uplink_power.h"),
        ("UplinkPower",            257, 379, N + "uplink_power.h"),
        ("build_info_json",        571, 631, N + "stats_fill.h"),
        ("build_health_json",      633, 657, N + "stats_fill.h"),
    ],
    "2c step 2 — the RX run loop": [
        ("run_rx", 1310, 3970, "node/src/rx_node.cpp"),
    ],
    "Phase 3 prep — Loaded's loader": [
        ("load_all", 276, 323, "node/include/wblink/node/load.h"),
    ],
}

# The complete set of permitted edits: the storage class a definition needs
# once it lives in a header, and the continuation-line realignment that the
# wider prefix forces on a multi-line signature. A move needing an edit that
# is not listed here is not a move.
EDITS = [
    ("CsaParams csa_params(", "inline CsaParams csa_params("),
    ("VcmdParams vcmd_params(", "inline VcmdParams vcmd_params("),
    ("bool channel_allowed(", "inline bool channel_allowed("),
    ("QuietGapPolicy quietgap_policy(", "inline QuietGapPolicy quietgap_policy("),
    ("uint8_t vcmd_id_for(", "inline uint8_t vcmd_id_for("),
    ("const char* vcmd_name_for(", "inline const char* vcmd_name_for("),
    ("std::optional<uint8_t> mtu_tier_for_mode(const std::string& mode,\n"
     "                                         uint16_t supported) {",
     "inline std::optional<uint8_t> mtu_tier_for_mode(const std::string& mode,\n"
     "                                                uint16_t supported) {"),
    ("bool frame_is_eob(", "inline bool frame_is_eob("),
    ("bool frame_is_paced_eob(", "inline bool frame_is_paced_eob("),
    ("bool frame_is_live_rtp_data(", "inline bool frame_is_live_rtp_data("),
    ("uint32_t session_nonce(", "inline uint32_t session_nonce("),
    ("std::array<uint8_t, kAnnouncePskSize> announce_token(",
     "inline std::array<uint8_t, kAnnouncePskSize> announce_token("),
    ("static std::string power_tier_json(", "inline std::string power_tier_json("),
    ("std::string build_info_json(const Loaded& l, uint32_t session,\n"
     "                            const char* role,\n"
     "                            const InfoSelfState* self = nullptr,\n"
     "                            const AirBackend* air = nullptr) {",
     "inline std::string build_info_json(const Loaded& l, uint32_t session,\n"
     "                                   const char* role,\n"
     "                                   const InfoSelfState* self = nullptr,\n"
     "                                   const AirBackend* air = nullptr) {"),
    ("std::string build_health_json(", "inline std::string build_health_json("),
    # 2c step 2. `run_rx` reached exactly one app-scope name on the way out —
    # the stop flag — so the whole 2661-line move is these two lines.
    ("int run_rx(const Loaded& l) {",
     "int run_rx(const Loaded& l, const std::atomic<int>& stop) {"),
    ("    while (g_stop == 0) {", "    while (stop == 0) {"),
    # Phase 3 prep: same header storage class as step 1's moves.
    ("int load_all(", "inline int load_all("),
]


def main():
    override = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "-" else None
    want = sys.argv[2] if len(sys.argv) > 2 else None
    dest = {}
    checked = fails = 0
    for step, blocks in STEPS.items():
        if want and want not in step:
            continue
        if step in VERIFIED_AT and not override:
            print(f"{step}: historical — passed at {VERIFIED_AT[step]}; "
                  f"the destination has changed since, so it is no longer "
                  f"re-checkable here")
            continue
        base = override or BASES[step]
        try:
            old = subprocess.run(["git", "show", f"{base}:app/main.cpp"],
                                 capture_output=True, text=True,
                                 check=True).stdout.splitlines(keepends=True)
        except subprocess.CalledProcessError as e:
            print(f"cannot read app/main.cpp at {base}: {e.stderr.strip()}",
                  file=sys.stderr)
            return 2
        print(f"{step}: {len(blocks)} block(s) against {base}")
        for name, first, last, path in blocks:
            if path not in dest:
                dest[path] = open(path).read()
            chunk = "".join(old[first - 1:last])
            applied = 0
            for src, repl in EDITS:
                n_hits = chunk.count(src)
                if n_hits:
                    applied += n_hits
                    chunk = chunk.replace(src, repl)
            checked += 1
            if chunk in dest[path]:
                print(f"  ok   {name}: {last - first + 1} lines, "
                      f"{applied} edit(s) applied")
                continue
            fails += 1
            print(f"  MISS {name}: app/main.cpp@{base}:{first}-{last} is not "
                  f"verbatim in {path}", file=sys.stderr)
            for line in chunk.splitlines(keepends=True):
                if line.strip() and line not in dest[path]:
                    print(f"       first differing line: {line!r}",
                          file=sys.stderr)
                    break

    print(f"move-identity: {checked} block(s) checked, {fails} not verbatim")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
