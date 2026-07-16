# Follow-up plan — pending rulings worked one at a time

Working register for the items raised during the Pass 36–39 controller
build-out (spatial cache repair, horizon frame caps, JSCC enforcement, FPS
ladder). Source of truth for the ruling *history* stays `review-log.md`;
this file tracks execution order and status. Update the status line when an
item lands; add the pass number.

Operator sequencing (2026-07-16): the easy locks first (R-D width lock,
high-cadence ARQ cutoff — both Pass 40), then R-A, then reassess.

| id | item | status |
|---|---|---|
| R-D | Dynamic 20/40 MHz width | **RULED (Pass 40):** v1 is fleet-wide 20 MHz; 40 MHz revisited later behind a hardware verdict + CSA-shaped design |
| — | High-cadence ARQ cutoff | **RULED (Pass 40):** no ARQ above 100 fps (101–144); 10 ms is the lowest comfortable recovery window. `policy.arq.arq_max_fps` seed 100 |
| R-A | LINK_REPORT preferred/latch gate (§3.5 gap) | **DONE:** preferred filter + first-latcher with silence re-latch ahead of the selector and fps ladder (code catching up to spec — no new ruling needed) |
| R-B | §14.2 enforcement × §14.3 cache parity offload | NEXT candidates — measure on a combined bench (cache + enforce on one stream); if TX parity migrates onto the cache, rule source-kind exclusion in the RX demand estimator |
| R-C | §14.3 repair window at high fps | OPEN — fps-sweep the cache bench (90/120/144); only add opt-in `max_blocks_ahead=1` if replies lose the supersession race in numbers |
| R-E | venc volatile writes (`persist=false`) | OPEN — venc-repo HTTP contract change; wanted before long flight soaks |
| R-F | Decoder-side deadline telemetry | OPEN — additive §15.3 RX late/deadline-miss counters first; enables the deferred §13.4 emergency fps path |
| R-G | fps ladder `max` (above-preferred) | DEFERRED — needs R-F evidence |
| R-H | RF cache transport ordering | DEFERRED — re-argue Pass 36 rule 8 only when an RF cache binding is proposed |
| — | §10 ground-uplink power scope | OPEN — recommend config-load warning for `power_map` on an rx-node uplink now; real control only if gate 4 shows return-margin problems |
| — | JSCC production flip | OPEN — radio-backend shadow soak during the gate campaigns; flip P-frames first; confirm discard visually before flight |
| — | Rig verification | OPEN — `ssc338q` cross-build, venc `make lint`, and radio/kernel-monitor re-runs of the four UDP harnesses (`cache_repair` / `actuation` / `jscc_enforce` / `fps_ladder`) |
| — | venc PR #181 housekeeping | OPEN — VERSION/HISTORY bump (merge-order dependent vs #178/#179), Star6E IDR-failure return code, old-SDK soft-enforcement log line |

Full problem statements and recommendations: `review-log.md` "Open
questions" register (2026-07-16).
