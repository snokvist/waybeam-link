# waybeam-link Roadmap

Follow-up work and near-term direction. This lives repo-local because
waybeam-link is a standalone repo, not a `waybeam-coordination` submodule
(see `CLAUDE.md`). Spec-bearing items still go through `PROTOCOL.md` + a
numbered `docs/review-log.md` Pass; this file only tracks *intent*.

## Active

- [x] **Multi-adapter scout hardening (Passes 64–66) — MERGED.** The Passes
  58–63 scout/claim foundation landed in PR #29; two-adapter on-device
  verification then fixed three decision-critical details: the uplink is the
  roaming scout while diversity ears hold the resting channel; a
  claim/abort/stop retunes or rolls back every adapter together; and candidate
  channels are selected by heard-most frame count so retune-settling leakage
  cannot send a claim to the adjacent channel.
- [ ] **Register control-plane ports in the coordination port registry.**
  Add `:8091` (REST control, PROTOCOL.md §15.5), `:8099` (fleet-monitor
  HTTP/SSE), `:9110` (stats NDJSON egress default) to
  `waybeam-coordination/protocols/port-registry.md`. No collisions found
  (8060 = hub WebUI, 8070 = flashd are the only nearby users).

## Planned

- [ ] **TX-power override knob** (`POST /api/v1/tx/power`). Deferred from the
  §15.5 core write set: a raw power write fights the §9/§10 per-tick selector
  power write, so it needs an explicit override-latch (set → selector yields
  until cleared) rather than a one-shot poke. Spec the latch semantics before
  coding.
- [ ] **Dashboard direct-to-instance mode.** With the REST plane live, the
  dashboard can pull each instance's native `GET /api/v1/stats/stream` (SSE)
  directly on the LAN instead of only via the `link_monitor.py` UDP-push
  bridge. Bridge stays the zero-config fleet default; direct mode is opt-in
  for single-instance deep-dives.
- [ ] **Control-plane auth posture (if exposed beyond LAN).** Current ruling
  is LAN-bindable, no auth (operator decision, Pass 16). If the surface ever
  needs to cross a trust boundary, add a bearer-token / mTLS mode — spec
  first, keep the no-auth LAN default.
- [ ] **Wider write surface as demand appears.** Candidate live knobs beyond
  the core set: ARQ `airtime_frac` / `attempt_cap`, return-window timing,
  per-adapter enable/disable. Add on evidence of a real tuning need, not
  speculatively — each new knob is a new MUT class to spec and test.

## Completed

- [x] **Ground scout + quickconnect foundation** — ANNOUNCE discovery,
  announced-token/secret key provenance, list-mode channel sweep, occupancy and
  candidate reporting, and CSA claim/channel-hold lifecycle. Passes 58–63,
  PR #29 (2026-07-20).
- [x] **Dashboard frame-SHM health** — fleet cards now expose fast/FEC/
  unrecoverable/malformed/decode outcomes, supersession/deadline drops, all
  three SHM backpressure counters, and adapter filter/kernel-drop diagnostics.
- [x] **UDP broadcast/sniffer follow-up** — hardened paced RX-only injection,
  added a real two-node bidirectional shared-channel test, and verified foreign
  delivery, socket-level self filtering, paced TX accounting, and zero false
  kernel-overflow counts.
- [x] **Fleet monitor** — `tools/link_monitor.py` + `link_monitor.html`,
  UDP-NDJSON → HTTP/SSE bridge, per-instance cards. (PR #18, 2026-07-12)
- [x] **REST control plane §15.5** — in-loop single-threaded HTTP server;
  read (`/stats`, `/stats/stream` SSE, `/info`, `/health`) + core write set
  (`/csa`, `/link/profile`, `/fec`, `/stats/reset`); stdin `csa` trigger
  removed. (PR #19, 2026-07-12)
- [x] **Frame-shm FEC stats mapping** — `FrameReassembler` counters folded
  into §15.3 `StreamStats`; added `frames_fast` / `frames_unrecoverable` /
  `malformed`; `recovered_fec` now non-zero on frame-shm streams.
  (PR #19, 2026-07-12)
