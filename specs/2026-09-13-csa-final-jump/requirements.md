# CSA final jump — remove the verify/revert handshake

**Status:** proposed · **Owner:** operator + Claude · **Opened:** 2026-09-13
**Tier:** 1 (§11 contract change — needs a numbered Pass in `docs/review-log.md`)

## Problem

§11's channel switch is a two-sided *verified* handshake. Both ends jump at
`T_switch`, each then waits for evidence of the other, and **each backs out
alone** if it does not see that evidence in time:

- craft — `core/src/csa.cpp:171`: *"§11.5 jump-failed backout: the retune landed
  on a dead channel — revert to prev_chan"*
- ground — `core/src/csa.cpp:463`: *"issuer revert-on-no-video"*

Two independent timers deciding the same question from different evidence
means the two can disagree, and when they do, one reverts while the other holds
(`State::kCommitted` = "hold the channel until reboot"). **Stranding is not a
bug in this design, it is the design.** `node/src/rx_node.cpp:1556` records
exactly that outcome for retune class 1: *"the craft reaches COMMITTED on the
target while the issuer reverts"*.

Measured on the bench 2026-09-12/13: **class-0 campaigns land 0 of 4** across
two dies and two crafts, against 26/26 recorded historically. The revert is
currently taking a ground *away* from a craft that had already committed. The
machinery is not buying safety; it is causing the failure it exists to prevent.

It also imposes a hard real-time deadline on the retune, which is what makes a
slow radio a protocol-level problem: an MT7612U's measured 789 ms retune
exceeds the 300 ms class-0 budget, and because `AirBackend::retune_all` is a
sequential loop over every adapter, **one slow diversity ear puts the whole
node out of contract** (measured 1643 ms on a 3-ear ground whose uplink retunes
in 41 ms). That is Pass 201.

## What we are changing

**The jump becomes final.** Both ends agree on `(channel, T_switch)`, both go,
and both stay. No verify window, no revert, on either side.

Recovery on a missed jump is **re-acquisition, not backout**: the craft keeps
transmitting wherever it is, on a channel inside the shared allowlist, so the
ground can always re-find it by scanning.

## Key decisions

| Decision | Choice | Why |
|---|---|---|
| Craft behaviour on a missed jump | **Stay put** on the target | Operator ruling 2026-09-13. Always findable by scan; a "return home after N s" rule reintroduces a craft-side timer, which is the class of thing being deleted. |
| Ground behaviour on a missed jump | Stay on the target, then re-acquire by scout | Symmetric with the craft; no timer, no special case. |
| Recovery cost | Bounded by a **priority scan**: previous channel and campaign target first | After a failed jump only those two are plausible. Turns a 10.9 s (Realtek) / 33.6 s (MT7612U) sweep into ~1 s. |
| Retune class 0 vs 1 | Collapses | Classes exist only to size a deadline. With no deadline there is nothing to size. |
| `dt_to_switch_ms` | Kept, generous, single value | Still needed to agree *when*. No longer a budget anyone can miss, so it can be set long enough for any adapter without a per-die model. |
| Per-die timing rules | **None** | Explicit operator requirement: no special rules per adapter. Pass 201's constraint dissolves rather than being mitigated. |

## Non-goals

- Parallelising `retune_all`. Worth doing on its own merits (cost becomes max,
  not sum) but it is an optimisation once the deadline is gone, not a fix.
- Pre-positioning / straddling ears during the lead. Same: a nicety once there
  is no deadline to miss. Keep it out of the first increment.
- Widening `dt_to_switch_ms` from a measured per-adapter retune cost. This
  supersedes that idea; it was a min-max approach to a deadline we are deleting.
- Any change to the CSA packet layout. `dt_to_switch_ms` and the channel fields
  stay as they are on the wire.

## Constraints and risks

- **Tier-1, both ends.** A new ground against an old craft leaves the craft
  still reverting underneath it. The craft-side change is a *deletion*, which
  is the safe direction, but the fleet must be updated together. `.232` is the
  only craft currently available.
- **In-flight outage on a missed jump** is the real cost: video is lost until
  re-acquisition. The priority scan is what bounds it, so it is part of this
  work, not a follow-up.
- **A forged or mistaken CSA is now unrecoverable without a scan.** Today's
  revert is an accidental backstop against a bad campaign. §11.4's
  authentication is what actually guards that, and it is unchanged — but this
  removes the second line of defence, and that should be stated in the Pass
  rather than discovered.
- ~~**Blocked on a separate craft-side bug.**~~ **RESOLVED.** The intermittent
  `no CSA_ARMED` / `video=0` on a clean Realtek ground was **stale craft state**
  — the craft's hub had been up since 2026-08-30. After a restart the same
  ground confirmed 3/3. Increment 0's counters are what showed it; see
  `docs/findings.md` 2026-09-13 ("the 0/4 was STALE CRAFT STATE").
- **A missed campaign no longer self-heals, and that is a real cost.** The
  revert being deleted means a ground that jumps without its craft stays split
  until someone re-acquires. Under the old design the same miss corrected
  itself silently. This is the trade being made deliberately; Increment 3
  bounds the recovery and Increment 4b makes the need for it visible.

## Definition of done

1. No verify deadline and no revert path on either side; the state machine has
   no "back out" edge.
2. A campaign issued on a ground with an 800 ms diversity ear lands, with no
   per-die rule anywhere in the code.
3. A deliberately missed jump recovers by scan in ~1 s, not a full sweep.
4. §11 amended and a numbered Pass recorded, naming the lost backstop.
4b. A jump the craft did not confirm is REPORTED as unconfirmed, not as a
   committed selection — the ground stays on the target but never claims a link
   it does not have (Pass 203, added after the first device run reported three
   unfollowed jumps as "campaign confirmed").
5. Device-verified on `.232` across both ground compositions (Realtek-only and
   with MT7612U ears present). **Realtek-only: DONE — 5/5 convergence,
   2026-09-13. MT7612U-ears arm: NOT done** (an MT7612U wedged its MCU during
   the session); the MT7612U diversity path itself is unregressed —
   `diversity/uniq` 1.99 with retunes re-measured at 809 / 788 ms.
