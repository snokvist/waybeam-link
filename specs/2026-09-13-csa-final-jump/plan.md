# Plan — CSA final jump

Increments are ordered so each one is independently checkable and the risky
protocol edit lands only after the bench can tell success from failure.

## Increment 0 — unblock attribution (PREREQUISITE, not optional)

Campaigns currently fail on a clean single-adapter Realtek ground with the
craft healthy and claimed: intermittent `csa: aborted (no CSA_ARMED)` and
`armed=1 landed=1 video=0`. Until that is understood, **any** result from the
rework is unattributable — a pass could be luck and a fail could be this.

- The craft logs no CSA events to `/tmp/waybeam.log` and its `:8091` is not
  listening, so craft-side CSA state is currently **unobservable**. Getting
  that visibility is the first task, not the fix.
- Ruled out already: craft channel allowlist (identical 25 channels, target
  included), craft not claimed (`claimed True by 9` after latch), ground verify
  window too short (widened 500 → 3000 ms, still fails).
- **Verify:** a campaign's outcome can be explained from craft-side evidence,
  not inferred from the ground's verdict alone.

## Increment 1 — delete the ground's revert

`core/src/csa.cpp` issuer path (~:463) and its `IssuerAction::Kind::kRevert`
consumer in `node/src/rx_node.cpp:3244`.

- Ground commits at `T_switch` and stays. `kRevert` is removed from the issuer
  state machine rather than left unreachable.
- `CampaignIntent` (acquire-parks vs retune-reverts, Pass 199) collapses: with
  no revert there is one posture.
- **Verify:** on a mixed ground with MT7612U ears, a class-0 campaign no longer
  reverts. `landed` is expected to read 1 regardless of ear composition, since
  the deadline it measures is gone.

## Increment 2 — delete the craft's revert

`core/src/csa.cpp` follower path (~:171), the `verify_deadline_us_` it fires
on, and `CsaAction::Kind::kRevert`.

- Craft commits and holds (stay-put ruling).
- `verify_timeout_ms` and its `rx_liveness_ms` ordering constraint
  (`io/include/wblink/config.h:351-356`) lose their meaning for CSA. Decide
  per-knob: delete, or keep for the unrelated RX-liveness guard.
- **Verify:** craft holds the target after a jump the ground deliberately
  misses (bench: issue a campaign with the ground's radio parked elsewhere).

## Increment 3 — priority scan, so a missed jump is cheap

`node/src/rx_node.cpp:1292` — the scout currently walks
`policy.csa.channel_allowlist` in config order and nothing persists prior
sightings.

- Order: last-known craft channel, then the last campaign target, then the
  rest of the allowlist.
- Independent of the CSA rework and useful on its own — a 25-channel sweep is
  10.9 s on an 8812AU and 33.6 s on MT7612U.
- **Verify:** after a deliberately missed jump, re-acquisition completes in
  ~1 s rather than a full sweep.

## Increment 4 — spec and Pass

- Amend §11.2/§11.5/§11.6 for the final-jump semantics; delete the class-0 vs
  class-1 budget text, which no longer describes anything.
- **Pass 202**, naming explicitly: the lost revert backstop against a bad
  campaign, and that §11.4 authentication is now the only guard there.
- Close Pass 201 by pointer — its constraint dissolves rather than being fixed.

## Increment 4b — the close must report itself (ADDED after the first device run)

Not anticipated. Deleting the issuer's revert made `kSuccess` fire whether or
not the craft followed, and `rx_node.cpp:3266` kept reporting it as
`campaign confirmed` / `selection_state = "committed"` — so a ground that had
just jumped away from its craft told the operator it held the link. Three
campaigns ran that way before the bench noticed.

- `kSuccess` reads `issuer.evidence().video_seen`. Confirmed → unchanged.
  Unconfirmed → hold the target (no retreat) but report `select_failed` and log
  `campaign UNCONFIRMED (armed/landed/video)`.
- `CsaFollower`'s §11.6 beacon exit gains a counter (`csa_beacon`, §15.3). It
  was the one uncounted `return false`, and it is the exit a craft takes when
  the issuer has already jumped without it — which is why an all-zero counter
  set was read as "nothing is arriving" when the truth was "a split pair".
- **Verify:** a campaign the craft cannot follow produces `UNCONFIRMED` and
  `select_failed`, and the craft's `csa_beacon` climbs while `csa_accepted`
  stays flat. Both were unobservable on the first run.
- **Recorded as Pass 203.**

**Consequence this increment does NOT remove.** The old revert self-healed a
missed campaign: the ground returned to the craft, so the next campaign was
always issued co-channel. The final jump deletes that, and re-acquisition is
operator-triggered. Increment 3 makes it ~1 s when run; nothing runs it
automatically. Auto-scout on an unconfirmed close is an operator decision and
is deliberately not invented here.

## Increment 5 — fleet

Ground and craft must move together; a new ground against an old craft leaves
the craft reverting underneath it. `.232` is the only craft available, so the
rollout question is real and belongs to the operator.

## Out of scope (recorded so they are not rediscovered)

Parallelising `retune_all`; straddling/pre-positioning ears during the lead;
sizing `dt_to_switch_ms` from measured retune cost. All three were live
proposals; all three become optimisations once the deadline is gone.
