# Follow-up plan — pending rulings worked one at a time

Working register for the items raised during the Pass 36–39 controller
build-out (spatial cache repair, horizon frame caps, JSCC enforcement, FPS
ladder). Source of truth for the ruling *history* stays `review-log.md`;
this file tracks execution order and status. Update the status line when an
item lands; add the pass number.

Operator sequencing (2026-07-16): the easy locks first (R-D width lock,
high-cadence ARQ cutoff — both Pass 40), then R-A, then reassess.

**Backend note (2026-08-10).** Pass 164 deleted the `kernel-monitor` backend;
devourer is the only RF backend. Rows measured on kernel-monitor are marked
**[HISTORICAL — kernel-monitor]** and state what, if anything, still carries
over. A DONE row on the retired backend is a record of a past run, **not
coverage of the shipping path** — read the mark before citing a row as
evidence.

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
| R-H | RF cache transport ordering | DEFERRED — Pass 50 gives UDP/IP a measured 3 ms first-NACK lead; re-derive grace/priority when an RF cache binding is proposed |
| — | §10 ground-uplink power scope | **RESOLVED (Pass 43):** `power_map` on an rx-node is rejected at config load; real return-power control remains gate-4-dependent |
| — | JSCC production flip | OPEN — radio-backend shadow soak during the gate campaigns; flip P-frames first; confirm discard visually before flight |
| — | UDP controller soak | **DONE:** `tools/controller_soak_udp.sh` — all controllers at spec seeds through clean→marginal→burst→fade→interference→outage→recovery; clean-shutdown/ASan, schema, write-budget, delivery, and full-recovery assertions. `SOAK_MULT` stretches phases for long runs |
| — | Physical gate-2 walk fade | **[HISTORICAL — kernel-monitor] DONE (Pass 47):** N=2/MCS5/FEC10 reduced 86‰ pre-diversity loss to 24‰ post-diversity loss; FEC recovered 599 source symbols versus 52 by ARQ. Longest joint blackout 37.1 s; keep 10% base, do not use static 33%. **This is the §17 gate-2 verdict and the 10% FEC seed rests on it** — see the gate-2 transferability question below |
| — | Stationary N=2 radio soak | **[HISTORICAL — kernel-monitor] DONE (Pass 48):** 9,000/9,000 decoded, 101 IDRs, byte-clean/EOS pass; no in-window SHM/kernel drop or adapter stall/wedge; FEC 123 source symbols versus ARQ 10. ("radio soak" here means real RF, **not** `air.kind: "radio"` — the ears were monitor netdevs `229b`/`2308`.) Carries over: the decoder/byte-exactness assertions, which are above the backend |
| — | Stationary receiver failover | **[HISTORICAL — kernel-monitor] DONE (Pass 48):** `2308` sustained RX + return while `229b` was down; full monitor reinitialization is required for CU failback, after which the running process clears `adapter_stalled` and reuses it without restart. **The failback mechanism is void** — there is no monitor sequence to reapply; devourer's equivalent is the §11.6 RX-path restart. **Needs a devourer re-run** |
| — | `229b` return-TX diagnosis | **[HISTORICAL — kernel-monitor] RESOLVED (Pass 48/49):** AF_PACKET `send()` succeeded 915 times while netdev TX counters did not advance and no RF aired. **The `tx_wedged`-from-netdev-TX-progress detector went with the backend.** What survives is the §9.10 principle — submission success is not TX liveness — now carried on devourer by CCX `tx_progress` and measured there directly in **Pass 168** (induced wedge cleared 5/5 by in-process backend rebuild; do-nothing control 0/5) |
| — | Independent ARQ cache | **[HISTORICAL RF, LIVE LOGIC — Pass 48]** real monitor cache + 150‰ N=1 aggregator stress reduced unrecoverable frames 534→119 (−77.7%) with zero rejected replies. Resend load stayed ~0.92/frame because cache + ARQ are parallel. The cache control path is Ethernet/localhost UDP-IP and is backend-independent; **only the RF ears that supplied the loss were monitor** |
| — | Cache timing/ordering evidence | **[HISTORICAL RF, LIVE SEED — Pass 50]** real P95 first reply/completion 2.845/2.910 ms; a targeted 3 ms first-NACK grace cut NACK packets 22.5% and vehicle resends 21.3% in clean 1,800-frame A/B runs. Default 3 ms, range 0..6, exact block + first NACK only. **The 3 ms seed measures an Ethernet/localhost round trip, not the air** — RF only supplied the loss pattern — so it transfers; recorded here so the provenance is not lost |
| — | Controller/cache regression rerun | **DONE (Pass 52), backend-independent:** run on **UDP-air**, not RF — both cache modes, cache-offload parity invariant, JSCC enforcement, FPS ladder, and the 158 s all-controller soak. Unaffected by the retirement |
| — | Kernel-monitor actuation | **[VOID — the actuator was deleted] (Pass 52):** intended 8812EU TX→8812CU RX path with host fake-venc; profile 5 clean and 5→3→5 loss/recovery, zero actuator failures. Residual-load ARQ P95 5.195 ms; forced 200‰ load saturates the vehicle resend queue at 16.039 ms P95. **Pass 164 deleted the `iw`-forked power actuator and its §10.5 absolute-reference row**; devourer actuates in offset space (§10.5, Pass 150). The ARQ P95 numbers stand as loading measurements; the actuation result does not describe any shipping path |
| — | Remaining rig verification | **REOPENED (2026-08-10).** The previous text — "cache repair and actuation now have real kernel-monitor coverage" — described coverage that no longer exists on the shipping backend. Devourer re-runs still owed: **(a)** receiver failover/failback, whose monitor mechanism is void; **(b)** cache repair with devourer RF ears; **(c)** JSCC enforce + FPS ladder, never RF-run on any backend; **(d)** the §17 gate-2 ρ verdict — see below. FPS validation must either actuate a volatile venc endpoint or keep the physical source inside every tested rung's capacity |
| — | §17 gate-2 transferability | **OPEN — operator ruling wanted (2026-08-10).** Gate 2 is marked PASSED in `step11-bench.md` §4.1 and `frame-fec-plan.md` checks the 10% FEC operating point off against it, but the only physical measurement is Pass 47 on the deleted backend. The argument for transfer is that ρ is a property of antenna geometry and the channel — two co-located ears fade together regardless of how the host reads them — and the backend moves *absolute* loss, not the *correlation* between ears. The argument against is precedent: Pass 139 refuted the monitor-era "devourer cannot do MCS4+" premise on hardware. **Either re-run the walk on devourer or rule the verdict transferable in writing; do not leave it cited unqualified** |
| — | venc PR #181 housekeeping | OPEN — VERSION/HISTORY bump (merge-order dependent vs #178/#179), Star6E IDR-failure return code, old-SDK soft-enforcement log line |

Full problem statements and recommendations: `review-log.md` "Open
questions" register (2026-07-16).
