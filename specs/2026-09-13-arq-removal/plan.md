# Plan — remove the NACK-based ARQ / retransmit plane

**Status:** design for review. Not yet a spec amendment; no code changed.
**Revision 2** — folded in three independent Flash reviews (see §13).

## 1. Goal and rationale

Remove NACK-based ARQ (lost-seq detection → NACK bitmap → resend from a ring)
as a repair mechanism. Keep **FEC** (§14.1), **spatial cache repair** (§14.3),
**decoder RECOVERY_REQUEST / venc IDR recovery** (§3.9), **per-adapter
diversity** (§6), and **slice concealment / GDR** (§6.3b) as the repair plane.

**Why.** The operator runs slice-based / intra-refresh (GDR) video. The only
strong case for ARQ was DPB repair by a late referenced-frame retransmit
(§14.1a); a GDR stream heals at the next refresh, so that value is gone.
Measured: ARQ contributed to 115 delivered frames versus FEC's 3,687, and
`recovered_arq` (packet gaps) was 174 with 165/174 within one frame period;
1,179 NACKs bought those 174 gap fills; at 100 fps the
useful window is ~10 ms against a 7 ms p95 NACK RTT. It is return-path
dependent (a receiver-only node cannot use it; a single-adapter ground blinds
its own RX to send it) and drags a large cross-cutting contract surface.
**FEC + diversity + cache + GDR concealment is the whole repair plane.**

This is **Tier-1**: it deletes wire types/bits, state-machine behaviour, and
config semantics. O1–O7 (§11) MUST be ruled before the spec-first commit.

## 2. Scope boundary — what stays

| Mechanism | Spec | Stays because |
|---|---|---|
| Forward FEC (GF(256) Cauchy RS) | §14.1 | One-way, no return path |
| Non-referenced frame class + `e_rate` | §14.1a | FEC policy |
| Spatial cache repair | §14.3 | Own RECOVERY_REQUEST/CACHE_REPLY transport |
| Decoder RECOVERY_REQUEST (ask encoder for IDR) | §3.9 | Not symbol retransmit |
| JSCC controller | §14.2 | FEC allocation (but see O6) |
| Return path / quiet gap / LINK_REPORT | §7 | Reports + adaptation |
| Per-adapter diversity + liveness | §6.1/§6.5 | Primary redundancy |
| Slice concealment / GDR | §6.3b | Real residual-loss cover |
| Hardware 802.11 retry (`air.tx_retry_limit`) | §3.0 | Different plane |
| `loss_postdiv_prearq` | §3.7 | Selector loss input — keep field+name (§3) |

## 3. Wire changes

- **DATA `data_flags`** (`core/include/wblink/types.h:45-53`): retire `kArq`
  (0x02), `kRetransmit` (0x04), `kPframeArq` (0x20) → **reserved, MUST send 0,
  MUST ignore on receipt**. Keep `kEndOfBlock`, `kFecRepair`, `kCsaArmed`. No
  DATA layout change, no `ver_type` bump.
- **`PacketType::kNack` (0x2)** (`types.h:18`): **reserved, never emitted**.
  Decode posture must be pinned (O-wire): 0x2 must decode to an **ignorable,
  non-fault** value. Today `decode()` returns `NackView` in the `Decoded`
  variant (`wire.h:318`, `wire.cpp:485-518`); removing `NackView` without a
  replacement sends 0x2 to `kUnknownType`. **Fix:** keep a reserved-type swallow
  (or a dedicated `kReservedNack` that both RX and TX drop silently) so an old
  peer's NACK is neither a fault nor an alarm.
- **Mixed-version matrix** (graceful iff §3-O-wire is done): new TX + old RX →
  old RX sees no ARQ bits, builds no NACKs; old TX + new RX → new RX never
  NACKs, TX ring idles; old RX NACK + new TX → dropped. **`kRetransmit`
  skew:** `rx.cpp:272-274` excludes retransmits from `note_adapter_seq` (→
  pre-diversity loss and gate-2 ρ). Removing it means an old TX's retransmits
  count as new packets during cutover. **Fix:** keep a decode-only
  `kRetransmit` swallow *and* the exclusion, or explicitly accept/document the
  skew.
- **`loss_postdiv_prearq`** stays on LINK_REPORT (`wire.h:78`,
  `wire.cpp:574`) — the selector input (`selector.cpp:292-332`). The repo has a
  rename-on-flip precedent (`docs/findings-pass3.md:487-489`,
  `docs/groundwork.md:38-43`). Because the meaning does not flip (it loses a
  qualifier), **keep the field and name, redefine it once as the delivered-loss
  boundary, mark `prearq` historical**, and never reuse the name for a
  different quantity.

## 4. Profile table and `table_version` (O4 — corrected)

`Profile.arq_deadline_iframe_ms` / `arq_deadline_pframe_ms`
(`table.h:34-35`) are hashed into the §3.6 canonical profile
(`table.cpp:43-44`, 27 B/profile). They are **also live**: `RxEngine::block_deadline`
(`rx.cpp:80-96`) sets the gap/drop budget, `TxCore::frame_deadline_us`
(`tx_core.h:310-320`) feeds JSCC, and `rx_node.cpp:625-626` sets the frame-shm
reassembly drop deadline. So they are neither removable nor inert.

**Corrected Option A (recommended):**
- **Keep the loader parsing them** (`config.cpp:1732-1734`) and the canonical
  bytes unchanged → **no `table_version` rotation** (Pass 82 precedent,
  PROTOCOL.md:616-624; the fields govern no cross-node behaviour, so divergence
  is harmless — unlike Pass 163's probe schedule).
- Stop using the **ARQ class** (`iframe_class` derived from the retired `kArq`,
  `rx.cpp:336-337`) and collapse to **one runtime deadline**, chosen from the
  existing fields (e.g. `pframe`, or `min(iframe,pframe)`), for gap/drop,
  frame-shm reassembly, and JSCC.
- "No runtime consumer" ≠ "stop reading": the loader MUST keep populating the
  hashed fields or the hash rotates and mixed peers fall to **sticky** §3.4
  best-effort until re-latch (PROTOCOL.md:515-519; not "for the window").

**Option B (collapse 27→25 B)** forces a lockstep rotation and a golden/vendored
recompute; defer.

**Pre-existing latent divergence to reconcile:** `tests/table_hash_test.cpp:57`
pins `0xC1` for a hand-built table with rung-4 airtime **510**, while the comment
claims it matches `profiles/table.example.json` (rung-4 **600**), which
`tests/config_test.cpp:1004-1007` hashes to **0xF2**. The "cross-check" claim is
false today; fix or document before any canonical-form work.

## 5. Configuration semantics

- Remove `streams[].arq_mode`, `policy.rx.renack_*`, `jscc_shadow.arq_guard_us`,
  `cache.repair.nack_grace_ms`, and most of `policy.arq` (parent + 13 children,
  not 12) — but **relocate, do not delete, `policy.arq.fwd_clamp_blocks`**
  (`config.h:276`): it is the §6.6 block clamp (`rx_core.h:51`,
  `rx.cpp:241,509`). Move it to `policy.rx` beside `fwd_clamp_pkts`; update
  registry `config_registry.cpp:319`, `config.cpp:853-854,1859`,
  `examples/config.tx.sample.json:25`, `config_test.cpp`.
- `policy.arq.classifier_size_threshold` → keep only if O2 keeps the RTP
  classifier; otherwise relocate to the FEC/RTP policy or delete.
- `vcmd_id::kArq` (0x01): retire the id (keep it **reserved**, do not renumber),
  remove `"arq"` from `vcmd.h:24,39`, and state that
  `wblink_rx_vehicle_command(rx,"arq",…)` / REST `{"cmd":"arq"}` now returns
  `REJECTED`.
- Loader semantics: unknown keys are silently ignored, and deploy `--strict`
  prints `strict: unknown key …` **without** failing `gates.sh`'s `(warning|error):`
  grep. So old configs keep loading; document it, and consider a `--strict`
  deprecation warning. `deploy/*.json`, `examples/*.json`, `profiles/*.json`
  are edited for correctness regardless.

## 6. Core removals

- **Delete** `core/src/scheduler.cpp` + `scheduler.h`, `core/src/ring.cpp` +
  `ring.h`.
- **`rx`:** `NackRequest`, `build_nacks`, `nack_*`/`arq_rec_*` counters +
  histograms, `Gap::nack_*`, `BlockInfo::arq`/`iframe_class`/`nack_attempted`/
  `first_nack_not_before_ms`, `recovered_arq`, `defer_first_nack`,
  `block_had_nack`, `block_deadline`'s ARQ-class branch.
  - **Entanglement (must preserve):** `Gap::nack_eligible` has a second role —
    `advance_cursor` (`rx.cpp:651-654`) drops a declared-lost gap as
    `dropped_unrecoverable` *immediately* when `!nack_eligible`, while
    ARQ-eligible gaps waited (`rx.cpp:570-571`). **Replace `nack_eligible` with
    an explicit "FEC-pending until deadline" condition** so a late FEC symbol
    can still complete the block; do NOT let every declared gap drop instantly.
  - Keep `fec_satisfied` as the completion concept, `dropped_deadline`,
    supersession, delivery, and the §6.5/§6.6 machinery.
- **`frame_framer`:**
  - Remove `FrameArqMode`, arq stamping, `arq_frames`/`arq_cutoff_frames`,
    `set_arq_suppressed`/`set_arq_enabled`, `allow_pframe_arq`.
  - **O1 (`min_k`):** the `k <= min_k && arq_eligible ⇒ r=0` rule
    (`frame_framer.cpp:52-54`) hands small frames no FEC *because ARQ would
    recover them*; with ARQ gone that is a bare frame (the B11 shape,
    unbounded). **Fix:** delete the `arq_eligible` parameter and branch so
    `r = max(ceil(k·rate), min_r)` for all classes; keep the `rate == 0 → 0`
    early return so `e_rate=0` stays deliberately bare; simplify the override
    term (`:154-155`) and jumbo-guard `arq_only` (`:163-174`).
    Re-derive the Pass-94/B11 boundary tests (`frame_framer_test.cpp:517-533,
    536-580,605-609,721-736`).
- **`framer`/`nal` (RTP ingress):** classifier + size heuristic exist only to
  set `block_arq_` (`framer.cpp:34-74`, `nal.h`). **O2:** delete the classifier
  (+ `streams[].classifier`) or keep the size heuristic as a future FEC input.
  Production is frame-shm; RTP is bench/loopback.
- **`jscc_controller` / `jscc_runtime_shadow`:** remove ARQ reason/gate
  (`kFecAndArq`, `kArqOnly`, `arq_capable`, `arq_eligible`,
  `resend_airtime_us`, `arq_guard_us`). **O6:** the shadow gates on
  `kRttReady` + `rtt_samples >= min_rtt_samples`
  (`jscc_runtime_shadow.cpp:53-57`), whose only source is the NACK RTT
  estimator populated on RETRANSMIT fills (`rx.cpp:288-306`,
  `rx_core.h:333-341`). Removing ARQ starves it → `kRttNotReady` forever →
  `jscc_valid_decisions`/`enforce` stop actuating. **Ruling required:** drop the
  RTT gate and base FEC allocation on `repair_demand` + floor/cap + airtime
  alone; amend §3.10/§14.2 feedback fields (`rtt_p95_us`, bit 1,
  `jscc_input_resend_us`, `jscc_output_arq_eligible`).
- **`frame_reassembler`:** remove `arq_sources`/`arq_repairs` and
  `frames_with_arq`/`frames_fec_after_arq` (`frame_reassembler.cpp:85-245`).
  Keep FEC decode and `frames_unrecoverable`; **`frames_fec_only` becomes the
  FEC-completion bucket** — add it on the FEC branch only (`:195-207`), not the
  fast path.
- **`cache_controller`:** remove `nack_graces_armed`, `blocks_repaired_before_nack`,
  `before_nack` (stat-only, `cache_controller.h:60-108`). Cache repair is
  request-driven and independent; `defer_first_nack`/`block_had_nack` are
  opportunistically coupled (verify per review 2).

## 7. Node / io / app / stats

- **`tx_core.h`:** remove `Stream::ring`/`sched`, `scheduler_policy`, `on_nack`,
  `drain_resends`, `note_live_bytes`, `arq_max_fps_`, `arq_fps_suppressed_`,
  `cmd_arq_enabled_`, `apply_command(kArq)`, ARQ stat fills, JSCC ARQ inputs.
  - **O3:** `report_authority_set/clear` calls `sched.force_lock/release_lock`
    (`tx_core.h:764-779`). The **report/feedback gates are not ARQ** and are
    consulted directly for LINK_REPORT/JSCC/verdicts/probes
    (`tx_core.h:450,499,521,551`); `arq_lock_holder` is stats-only
    (`tx_core.h:1192`). **Fix:** delete only the `s.sched.force/release_lock`
    loops; keep `report_gate_`/`feedback_gate_` and their `force_latch`/
    `clear_latch` calls bit-for-bit.
- **`rx_core.h` / `rx_node.cpp`:**
  - **O4-rename:** `inject_nack` is the shared return injector for ARQ NACKs,
    §3.9 RECOVERY_REQUEST, §3.9 latch bootstrap, and §3.10 JSCC_FEEDBACK
    (`rx_node.cpp:1010-1021,2293-2294,3033,3180`). Rename to `inject_return`,
    remove **only** `arq_timing.note_nack_*` and the `emit_nacks`/`repair_tail`
    gate (`rx_node.cpp:2967-2976,3021-3023`). **Keep the `urgent_ret_held`
    deferral and flush queue verbatim** — it is the shared quiet-gap pacing;
    deleting it would fire recovery/JSCC immediately into the craft's TX-deaf
    window.
  - Remove `ArqTimingTracker`/`arq_timing` (`stats_fill.h:37-201`),
    `arq_rx_enabled`, `/api/v1/arq`, `arq_enable`.
  - **Re-source the frame-shm reassembly deadline** (`rx_node.cpp:625-626`)
    from the retained unified profile deadline (§4).
- **`io`:** remove `ControlHandlers::arq_enable` + `POST /api/v1/arq`; ARQ config
  parsing; ARQ stats emission. **O5:** keep `AirIface::inject_resend` and both
  implementations (`air_udp.cpp:101-199`, `air_radio.cpp:1918-1953`) — the
  urgent-return lane carries non-ARQ returns too; remove only the ARQ producer.
- **`app/main.cpp`:** loopback `inject_nack` → `inject_return`; drop ring/
  scheduler includes. **Pre-existing bug found during review:** the loopback
  call `rx.tick(loop_now, deliver, inject_nack, inject_report)`
  (`app/main.cpp:488`) passes args **swapped** versus `RxCore::tick`'s
  signature `(now, deliver, inject_report, inject_nack, …)`
  (`rx_core.h:239-241`) — fix while here.
- **Stats (`stats.h`, `stats.cpp`, `stats_fill.h`):** remove `recovered_arq`,
  `arq_recovered_*`, `frames_with_arq`, `frames_fec_after_arq`, `nacks_sent`,
  `nack_rtt_*`, `arq_rec_*`, `resends_sent`, `arq_lock_holder`,
  `double_send_suppressed`, `arq_frames`, `arq_cutoff_frames`, `arq_timing`,
  cache `nack_graces_armed`/`blocks_repaired_before_nack`,
  `jscc_input_resend_us`, `jscc_output_arq_eligible`, `cmd_arq`,
  `arq_rx_enabled`. Keep FEC/JSCC/quality/loss fields.
- **C ABI / external:** stats are JSON strings (`rx_node_c.h:316`,
  `tx_node_c.h:207`), so this is a **telemetry-schema** break, not a C-layout
  ABI break. But `vcmd_id::kArq` is a real control input
  (`wblink_rx_vehicle_command("arq",…)`, `rx_node_c.h:262`), and
  **`GET /api/v1/features`** emits `arq_mode`/`arq_enabled`/`arq_effective`
  (`features.h:31-86`) — consumers branch on `arq_effective`. Coordinate with
  `waybeam-android`/`waybeam-hub`.
- **§3.4 best-effort (O7):** it currently means ARQ eligibility + supersession +
  deadline drops disabled (`stats.h:185-196`, `rx_core.h:617-644`). Latch and
  observability survive; **rule what it still disables** post-ARQ.
- **`--air-trace`** (`air_backend.h:126-160`) emits `arq`/`retransmit` flags and
  a `packet:"nack"` record — prune/relabel.

## 8. Tests, tools, data

- **Delete:** `tests/scheduler_test.cpp`, `tests/ring_test.cpp`,
  `tests/expand_arq_topology_test.py`, `tools/gate3_rtt.py`.
  **Remove their registrations** from `tests/CMakeLists.txt:36,38,180-182`
  (else configure/ctest fails) and fix the stale suite-count comments
  (`:223-239`; CLAUDE.md says both "71-suite" and "83 suites").
- **Add to the affected-test list:** `config_registry_test.py` (must move with
  the accessor/registry edits), `config_schema_test.cpp`,
  `config_strict_test.cpp`, and `selector_test.cpp` (O4-B only).
- **Rewrite/prune:** `rx_test`, `loopback_test` (**near-rewrite** — its harness
  is Ring+Scheduler+NACK, `:33-67`), `stats_test` (**inline golden strings**
  `:260-530`, no golden file), `config_test`, `frame_reassembler_test`,
  `frame_framer_test` (O1 boundary), `nal_test`/`framer_test` (delete if O2),
  `control_server_test`, `vehicle_cmd_test`, `jscc_controller_test`,
  `jscc_runtime_shadow_test`, `wire_roundtrip_test`, `wire_vectors_test`,
  `wire_fuzz_test`, `features_test`, `cache_controller_test`,
  `frame_shm_loopback_test`, `app_test`, `rx_runtime_control_test`,
  `air_staged_test`, `udp_test`, `uplink_data_test`, `node_rx_core_test`,
  `node_tx_core_test`, `table_hash_test` (only if O4-B), `reporter_test`,
  `mcs_probe_test`, `fps_ladder_test`, `link_monitor_test.py`,
  `jscc_replay_test.py`.
- **Tools:**
  - `tools/link_monitor.html` (not `.py`) holds the ARQ schema
    (`link_monitor.html:91-174`); `tests/link_monitor_test.py` reads the HTML.
  - `tools/expand_arq_topology.py` is **not ARQ** despite the name — it is a
    generic UDP TX/RX topology generator (`:37-109`) referenced by
    `docs/config-harness-plan.md` and `docs/bench-and-tools.md`. **Rename, do
    not delete.**
  - `tools/spatial_conceal/salvage_sim.py:171-176` models IDR ARQ — rewrite
    alongside `spatial_conceal_bench.cpp:181`.
  - Shell benches reading removed keys: `frame_shm_udp_bench.sh:110-256`,
    `jscc_ethernet_bench.sh:361-545`, `cache_repair_bench.sh:179`,
    `cache_offload_bench.sh:67-71`, `jscc_enforce_udp_bench.sh:59-73`,
    `controller_soak_udp.sh:66-70`, `actuation_udp_bench.sh:35`.
  - `gate2_rho.py` (drop ARQ fields), `jscc_replay.py` (drop `--arq`). **No
    change:** `mcs_power_sweep.py` (only `loss_postdiv_prearq_milli`).
- **Data:** `profiles/table.example.json`, `profiles/table-8733b.json`,
  `deploy/*.json` (`arq_mode`), `examples/*.json`, `profiles/modes/README.md:194`.

## 9. Documentation and README (explicit sweep)

- **`README.md`:** TOC (25,50), the "no MAC-layer ARQ" framing (77), "targeted
  retransmission" (85,91-94), config prose (117), "return path retransmission
  requests" (156-157,199), the "Loss recovery" section (233-251), telemetry
  (337), `core/` "scheduler" (443), "Retransmissions come back fast enough"
  (490-491), closing pitch (507,511). The pitch becomes **FEC + diversity +
  cache + conservative concealment**, and must state honestly that a repair
  path is removed, not just simplified.
- **`PROTOCOL.md`** — full sweep (reviews 1/3): delete §3.3, §4.1, §5.2, §5.3,
  §6.4, §8 retry clauses, §12; amend §3.0 (urgent ARQ/NACK lane 281-388), §3.1
  (type 0x2, 403-414), §3.2 (flags), §3.4 (486-519), §3.5 (549,554), §3.7,
  §3.8/§3.9 (713,718,824), §3.10 (§846-863 `rtt_p95_us`/bit1), §3.11 (896),
  §3.15/§3.16 (1240-1244), §4 (1322), §5.1/§5.1a (1378-1427), §6.2 (1482-1483),
  §6.3a (1519-1524), §6.3b (1600), §11.7 (4443), §13 (threat rows 4621-4634),
  §14 intro, §14.1 (`min_k`), §14.1a (4802-4835), §14.2 (rule 3), §14.3
  (5085,5168-5181), §15.2, §15.3, §15.5 (POST + `/features`), §16.3 (6919-6926),
  §17 (`retransmit airtime frac` 6941, `deadline budget (per class)` 6942,
  `arq_max_fps` 6951, gate 3), §18, §19; and the abstract/§1-2/§7 mentions.
- **`CLAUDE.md`:** line 7 ("opt-in importance-gated ARQ") **and** Layout
  (291-292 `core/` "ring, scheduler"; 382-383 `tools/ gate3_rtt.py`) and the
  stale suite counts.
- **`docs/`:** `review-log.md` (new Pass), `findings.md` (deletion finding +
  2026-09-13 measurement), `step11-bench.md`, `bench-and-tools.md`,
  `mon-air-verification.md`, `jscc-controller-review.md`,
  `waybeam-jscc-controller.md`, `venc-mode-matrix.md`,
  `transport-architecture-review.md`, `frame-fec-plan.md`,
  `devourer-integration-analysis.md`, `build-order.md`,
  `library-extraction-plan.md`, `followup-plan.md` (keep its
  "[HISTORICAL RF, LIVE LOGIC]" rows as history), `preflight-open-issues.md`,
  `groundwork.md`, `config-harness-plan.md` (update the `expand_arq_topology`
  references), `hwack-hybrid-bringup.md`,
  `ground-uplink-calibration-handover.md`, `spatial-concealment.md`,
  `scout-design.md`, `mcs-rung-lockout-plan.md`, `devourer-parity-plan.md`,
  `device-verification-plan.md`, `calibration-*.md`.
  **Frozen:** `review-log-archive-p001-152.md` and dated `findings*` history are
  not rewritten.
- **Coordination repo:** `repos/waybeam-link.md:4`, `roadmaps/waybeam-link.md`
  (207,219,271), `CLAUDE.md`; `ROADMAP.md` stub. (Not in this workspace — verify
  in the coordination checkout.)

## 10. Reallocation and behaviour change (bench-gated)

Removing ARQ frees airtime (`policy.arq.airtime_frac` 0.15) and removes a
protection path. Ship a **conservative all-k FEC rule in the core commit** (O1),
re-tune `p_rate`/`i_rate` to absorb the freed airtime, and make the FEC-only vs
ARQ-on A/B at ~120‰ and ~300‰ (healthy + weak uplink, CU rig) a **merge
precondition** — success = no regression in `frames_unrecoverable`/delivered at
matched airtime. Hardware is in use by another session; schedule it.

## 11. Rulings (operator, 2026-09-13 — all adopted as recommended)

**Ruled.** O1 all-k FEC (`r=max(ceil(k·rate),min_r)`, `rate=0` stays bare);
O2 delete the RTP classifier; O3 report/feedback authority preserved;
O4 keep the profile byte layout, no `table_version` rotation, one unified
runtime deadline; O5 keep the `inject_resend` transport, remove only the ARQ
producer; O6 drop the JSCC RTT gate and derive FEC from
`repair_demand`+floor/cap+airtime; O7 best-effort still disables supersession
and deadline drops; O8 reserved-0x2 ignorable swallow + keep the decode-only
`kRetransmit` exclusion. Original option text retained below for provenance.

### Open decisions (as proposed)

- **O1** Small-frame protection after `min_k` loses its ARQ premise: always FEC
  (`r=max(ceil(k·rate),min_r)`, `rate=0` stays bare)?
- **O2** Delete RTP NAL/size ARQ classifier + `classifier`, or keep as FEC input?
- **O3** Confirm report/feedback authority is decoupled from the §12 ARQ lock
  and preserved bit-for-bit.
- **O4** Keep the profile byte layout (no rotation; loader keeps parsing; one
  unified deadline) vs collapse (lockstep)? **Recommend keep.**
- **O5** Keep `inject_resend` transport, remove only the ARQ producer?
- **O6** JSCC RTT readiness loses its only source (NACK RTT): drop the gate and
  derive FEC from `repair_demand`+floor/cap+airtime? (§3.10/§14.2 amendment)
- **O7** What does §3.4 best-effort still disable once ARQ is gone?
- **O8 (wire)** Reserved-type decode posture for 0x2/`NackView`, and whether to
  keep the `kRetransmit` decode-only swallow + `note_adapter_seq` exclusion.

## 12. Suggested commit order

1. **Spec amendment** (PROTOCOL.md + review-log Pass + findings entry) with
   O1–O8 rulings folded in — first.
2. **Core** deletion + all-k FEC rule + unit-test updates (bench gate attached).
3. **Node/io/app/C-ABI wiring + config keys** (`fwd_clamp_blocks` relocation,
   registry sync).
4. **Stats schema** as its own commit/PR with external coordination
   (`waybeam-android`/hub) — telemetry-visible.
5. **Tools, data, configs.**
6. **README + docs + coordination repo.**
7. **FEC re-tune** (bench-gated).

## 13. Review disposition (three Flash reviewers, 2026-09-13)

Accepted and folded in: O4 loader/hash contradiction (all three); live-deadline
consumers `rx.cpp:80-96`/`tx_core.h:310-320`/`rx_node.cpp:625` (R2/R3);
`fwd_clamp_blocks` is a §6.6 clamp (R2/R3); JSCC `kRttReady` starvation (R2/R3)
→ O6; `Gap::nack_eligible` immediate-drop (R2); `kRetransmit`/`note_adapter_seq`
skew (R2); `inject_nack` shared path + `urgent_ret_held` (R2); report gate
decoupling (R2); CMakeLists registrations (R3); `link_monitor.html` (R3);
`expand_arq_topology.py` is generic (R3); shell benches + `salvage_sim.py` (R3);
`loss_postdiv_prearq` rename-on-flip (R1); 0x2 decode posture (R1); PROTOCOL/
README section gaps (R1/R3); stale `0xC1`/`0xF2` cross-check claim (R1);
pre-existing loopback inject swap (R2). No reviewer claim contradicted the
others; the three converged on O4 and on "do not delete the shared return
transport".
