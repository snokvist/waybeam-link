# Validation — CSA final jump

Every claim here has to survive the trap this workstream kept falling into:
**a control that does not remove the variable from the NODE is not a control.**
Three "8812AU controls" were run against the auto config, which still brought
up the MT7612U ears, so the thing under test was still present. Only a
single-adapter ground settled it.

## Bench matrix

| # | Ground composition | Why |
|---|---|---|
| A | 8812AU alone (array form, `bus` pin only) | The clean baseline. No MediaTek anywhere in the node. |
| B | 8812AU + 2× MT7612U (auto) | The composition this repo actually ships on `x86-ground`. |
| C | 2× MT7612U, no Realtek (array form) | Slow uplink *and* slow ears. |

Pin test adapters by **bus only**. With a `mac` present the pin degrades to a
claim preference (§10.6 D2) and silently binds whatever came up — that is how
an RTL8733B was measured and nearly reported as MT7612U. Read `part=` from
`/api/v1/info` before trusting any number.

## Per-increment checks

**Inc 0 — attribution.** Craft-side CSA state observable. Re-run the current
failure and explain it from craft evidence. Until this passes, no other row
below is interpretable.

**Inc 1 — ground revert deleted.** Matrix A/B/C, class-0 campaign:
- no `csa: selection reverted` in any run
- `landed` reads 1 on B and C, where it read 0 before
- **negative control:** with the change reverted, B reverts again. A fix never
  seen to change the outcome is not a fix.

**Inc 2 — craft revert deleted.** Issue a campaign the ground deliberately
does not follow (park its radio elsewhere). Craft must hold the target — check
by scanning for it there. Pre-change it returns to `prev_chan`.

**Inc 3 — priority scan.** Time re-acquisition after a missed jump, A and C.
Expect ~1 s against a 10.9 s / 33.6 s full sweep. Confirm the ordering is
actually used and not coincidence: a craft on the *last* allowlist entry must
still be found quickly when it is the last-known channel.

**Inc 4 — gates.** `scripts/gates.sh` green with all three toolchain env vars
set, or four cross gates skip silently and a skip is not a pass.

## Measurements to repeat, with their baselines

| Metric | Baseline (2026-09-12/13) | Expected after |
|---|---|---|
| class-0 campaigns landed | **0 / 4** (two dies, two crafts) | lands on A, B and C |
| `landed` flag, mixed ground | 0 | 1 |
| MT7612U full retune | 789 ms (n=27, 739–811) | unchanged — it stops *mattering*, it does not get faster |
| `retune_all`, 3-ear ground | 1643 ms (41 + 815 + 787, serial) | unchanged in inc 1–4 |
| Re-acquire after missed jump | full sweep 10.9 s / 33.6 s | ~1 s |

## Traps already paid for

- **Post-diversity loss and picture quality are not evidence** that diversity
  or a campaign is working — they read clean with one good ear. Use the
  explicit flags (`armed` / `landed` / `video`) and `diversity/uniq`.
- **`csa: acquire ABORTED (no CSA_ARMED)` is not automatically a failure** — on
  an acquire the ground CSAs itself onto the craft and an unclaimed craft never
  arms. Compare against the same campaign *class*, not across acquire/retune.
- **The claim log line `%u->%u` is `cand->chan -> target`**, not
  `current -> target`. It never shows the ground's own channel; do not read a
  same-channel claim out of it.
- **`retune_all` is serial**, so a campaign's retune cost is the sum across
  ears, not the uplink's. Any timing measurement must say which it means.
