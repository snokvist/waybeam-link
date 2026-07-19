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
| R-B | §14.2 enforcement × §14.3 cache parity offload | **RESOLVED (Pass 45 correction):** cache merges retain explicit air-only attribution; deterministic 120-block all-cache-completable coverage joins `cache_offload_bench.sh` as the regression gate |
| R-C | §14.3 repair window at high fps | **MEASURED, zero retention stands (2026-07-16):** 150 ‰ sweep — repair success 100 % @90 fps, 82 % @120, 62 % @144 (unrecoverable 5.7/5.3/8.9 %). Latency-first keeps zero retention pinned; the +1-frame knob is reconsidered only if a 144 fps cache-primary deployment shows the ~3 % delivery cost matters in flight (note: Pass 40 cuts ARQ >100 fps, so the cache IS the repair path there) |
| R-E | venc volatile writes (`persist=false`) | OPEN — venc-repo HTTP contract change; wanted before long flight soaks |
| R-F | Decoder-side deadline telemetry | **CLOSED (operator, 2026-07-16):** not load-bearing — late frames are dropped before the SHM boundary and already counted; reopen only on unexplained consumer-side latency in rig/flight data |
| R-G | fps ladder `max` (above-preferred) | **CLOSED with R-F** — `max` stays reserved |
| R-H | RF cache transport ordering | DEFERRED — re-argue Pass 36 rule 8 only when an RF cache binding is proposed |
| — | §10 ground-uplink power scope | **RESOLVED (Pass 43):** `power_map` on an rx-node is rejected at config load; real return-power control remains gate-4-dependent |
| — | JSCC production flip | OPEN — radio-backend shadow soak during the gate campaigns; flip P-frames first; confirm discard visually before flight |
| — | UDP controller soak | **DONE:** `tools/controller_soak_udp.sh` — all controllers at spec seeds through clean→marginal→burst→fade→interference→outage→recovery; clean-shutdown/ASan, schema, write-budget, delivery, and full-recovery assertions. `SOAK_MULT` stretches phases for long runs |
| — | Physical gate-2 walk fade | **DONE (Pass 47):** N=2/MCS5/FEC10 reduced 86‰ pre-diversity loss to 24‰ post-diversity loss; FEC recovered 599 source symbols versus 52 by ARQ. Longest joint blackout 37.1 s; keep 10% base, do not use static 33% |
| — | Stationary N=2 radio soak | **DONE (Pass 48):** 9,000/9,000 decoded, 101 IDRs, byte-clean/EOS pass; no in-window SHM/kernel drop or adapter stall/wedge; FEC 123 source symbols versus ARQ 10 |
| — | Stationary receiver failover | **DONE (Pass 48):** `2308` sustained RX + return while `229b` was down; full monitor reinitialization is required for CU failback, after which the running process clears `adapter_stalled` and reuses it without restart |
| — | `229b` return-TX diagnosis | **RESOLVED operationally (Pass 48):** AF_PACKET `send()` succeeds but netdev TX counters do not advance and no RF is emitted; keep RX-only. Add kernel-monitor silent-TX observability before treating submissions as health |
| — | Independent ARQ cache | **UDP/IP VERIFIED (Pass 48):** real monitor cache + 150‰ N=1 aggregator stress reduced unrecoverable frames 534→119 (−77.7%) with zero rejected replies. Resend load stayed ~0.92/frame because cache + ARQ are parallel |
| — | Cache timing/ordering evidence | **NEXT before RF cache:** add request→first-reply/completion timing; evaluate a fresh-cache-only bounded pre-NACK grace against the existing parallel policy. Do not carry the zero-RF-airtime v1 rationale into an RF binding |
| — | Remaining rig verification | After the stationary sequence, run radio/kernel-monitor coverage for the controller/cache harnesses (`cache_repair` / `actuation` / `jscc_enforce` / `fps_ladder` / `cache_offload`) and the controller soak |
| — | venc PR #181 housekeeping | OPEN — VERSION/HISTORY bump (merge-order dependent vs #178/#179), Star6E IDR-failure return code, old-SDK soft-enforcement log line |

Full problem statements and recommendations: `review-log.md` "Open
questions" register (2026-07-16).
