# JSCC controller architecture review and Ethernet bench

Review date: 2026-07-12. This reviews `docs/waybeam-jscc-controller.md` against
the current protocol and implementation. The investigative brief is useful
direction, but it is not yet an implementation specification.

## Findings

1. **The controller objective needs a measurable deadline definition.** A
   144 fps frame period is 6.94 ms, but it is not automatically the transport
   budget. Capture, encode, decode, and display consume parts of the same
   sensor-to-glass budget. The current SHM `pts` is a truncated, producer-local
   value and cannot establish cross-host one-way latency. First add a shared
   measurement convention (clock synchronization or an echoed/correlated
   capture identifier), and define the display deadline and percentile target.

2. **The brief's GF(256) limit is off by one relative to the protocol and
   implementation.** Waybeam's Cauchy construction permits `k + r <= 256`, not
   255 (`PROTOCOL.md` section 14.1). More importantly, current behavior does not
   enforce one frame = one protected block: when `k + r_target > 256`, it sends
   the frame as source symbols with FEC disabled and increments
   `fec_oversize_k`. The proposed invariant therefore needs an explicit policy:
   increase symbol size, reduce parity, reduce the source frame, or discard it.

3. **Packet-count signaling is already implemented.** Every source symbol has
   `window_len=k` and `sym_index`; every repair carries `window_len`, base
   sequence, and frame length. The roadmap should treat immediate partial-frame
   loss detection as verification work. Total frame loss still needs a paced
   expected-frame clock or sender announcement; the current receiver cannot
   infer a block for which no symbol arrived.

4. **Current deadlines are age-from-observation budgets, not display slots.**
   RX reassembly and ARQ already expire blocks and expose deadline counters, but
   the timer begins when a block is first observed. TX does not preflight a
   whole frame against an absolute display deadline, and cannot abandon an
   unstarted frame based on receiver display state. The proposed discard logic
   is new protocol/controller work, not an existing primitive.

5. **Per-frame encoder authority is unproven on SSC338Q.** Waybeam-link can
   command bitrate through the venc HTTP API, write-on-change, at selector
   timescales, and venc exposes an explicit local `GET /request/idr` actuator.
   There is no current per-frame target-size/QP interface. Disabling
   the encoder's rate control must be gated on a measured encoder experiment:
   command latency, frame-size response, quality bounds, buffer behavior, and
   whether the SDK safely accepts a frame-level QP path at 144 fps.

6. **The proposed control inputs are incomplete or differently scoped today.**
   The implementation exposes profile and FEC rate control, per-adapter radio
   telemetry, ARQ recovery histograms, frame outcomes, and SHM pressure. It does
   not yet expose a controller-ready frame-size distribution, absolute airtime,
   encoder actuation latency, display misses, or a fitted burst-loss model.
   Instantaneous RSSI should remain diagnostic; controller decisions need
   defined windows, estimators, confidence, and hysteresis.

7. **A UDP diversity simulation is an accounting test, not RF diversity.** Two
   craft UDP targets produce two receiver adapter observations and exercise
   merge/counter behavior. They share the same Ethernet path, so they cannot
   establish RF correlation, fade coherence, MTU/PER coupling, MCS behavior, or
   return-window physics. Synthetic per-adapter drop is suitable for deterministic
   controller replay and failure accounting only.

8. **The live encoder does not currently realize the brief's nominal 144 fps.**
   The first Ethernet baseline reported about 90 fps despite `video0.fps=144`,
   and marked no IDR frames, consistent with the intended GDR mode. Controller
   deadlines must use realized capture cadence and explain the configured-versus-
   realized gap before treating 6.94 ms as the operating interval. Zero IDRs in
   this passive baseline indicate steady-state GDR, not lack of an IDR request
   capability. The controller must specify a narrow IDR-request policy because
   an IDR is a large-frame exception to invariant I3.

## First hardware bench

`tools/jscc_ethernet_bench.sh` runs the real encoder path without touching the
radio backend:

```
SSC338Q waybeam -> venc_frame -> waybeam-link TX
    -> UDP :5801 + :5802 -> x86 diversity merge/FEC/reassembly
    -> venc_frame_out -> GStreamer H.265 decode
```

The runner deploys the current SSC338Q cross-build over
`/usr/bin/waybeam-link` and persistently installs its generated config as
`/etc/waybeam-link/jscc-ethernet.json`. It backs up `/etc/waybeam.json`,
temporarily selects `frame-shm://venc_frame`, restarts the encoder, and restores
the exact encoder config on exit. If that output is already active, it leaves
the encoder config and process untouched. Continuous mode runs until
interrupted and keeps the decoder plus dashboard active. Finite mode stores
configs, TX/RX NDJSON, stderr, decoded-frame validation, a per-frame
arrival/size/PTS CSV, and a JSON summary under `artifacts/` (gitignored).

Both nodes also push their 5 Hz stats to the x86 monitor on UDP port 9110. The
runner starts `tools/link_monitor.py` if it is not already running; view the
active vehicle and ground cards at `http://192.168.2.242:8099/`.

Prerequisites: passwordless root SSH to the craft, release and SSC338Q builds,
matching profile tables, and Ethernet addresses `192.168.2.201` (craft) and
`192.168.2.242` (ground). Run:

```sh
cmake --build --preset release -j
cmake --build --preset ssc338q -j
tools/jscc_ethernet_bench.sh start              # detached continuous bench
tools/jscc_ethernet_bench.sh status
tools/jscc_ethernet_bench.sh stop               # stops and restores venc
FRAMES=1440 tools/jscc_ethernet_bench.sh finite # foreground trace + summary
RX_DROP_PERMILLE=100 FRAMES=1440 tools/jscc_ethernet_bench.sh finite
```

Pass requires the requested number of valid H.265 frames, successful decode,
monotonic producer PTS, no malformed metadata/reassembly, and no unexplained
kernel socket drops in the no-loss run. The CSV establishes frame-size and
arrival-jitter distributions; it does **not** claim cross-host one-way latency.

### Initial results

The live bench passed twice on 2026-07-12 with 300 decoded frames per run:

| injected loss per path | post-diversity loss | fast | FEC recovered | unrecoverable |
|---:|---:|---:|---:|---:|
| 0% | 0% | 300 | 0 | 0 |
| 10% | 1.0% | 273 | 16 | 0 |

Both runs had valid metadata and Annex-B framing, monotonic PTS, successful H.265
decode, no SHM drops, and no kernel socket drops. Frames averaged about 6.4 KB,
but the maximum was 192-204 KB. Mean inter-arrival spacing was 13.2-13.4 ms
while P95 was about 12.6 ms; large-frame stalls dominate the tail above P95.
No IDR metadata was observed, as expected for the configured GDR mode.

### Frame-size operating envelope

The operator expects the 512 KB SHM slot ceiling to remain unreachable at the
intended 25 Mbit/s, 60--90 fps, intra-refresh operating points. A direct source
SHM measurement on the SSC338Q with `video0.size=auto` confirmed this for the
available 90 fps sensor mode:

| target | realized | frames | mean | maximum | slot use |
|---:|---:|---:|---:|---:|---:|
| 25 Mbit/s, 60 fps cap | 24.66 Mbit/s, 60.2 fps | 3,586 | 51,164 B | 74,789 B | 14.3% |
| 25 Mbit/s, 144 fps cap | 24.82 Mbit/s, 90.2 fps | 5,371 | 34,393 B | 62,242 B | 11.9% |

Both windows had valid metadata, Annex-B framing, monotonic PTS, and zero ring
lag. The 144 setting is only a ceiling; this sensor mode supports at most 90
fps. The 60 fps validator window contained no IDR and its legacy validator
therefore printed `FAIL`; that condition is expected for steady GDR and none of
its integrity checks failed.

At the standard `max_payload=1424`, the actual symbol data size is 1,387 B.
With the bench's 10% P-frame parity, the largest capacity-safe encoded frame is
321,776 B (`k=232,m=24`). At 25% IDR-class parity it is 282,940 B
(`k=204,m=51`). Thus FEC capacity, not the 512 KB SHM slot, is the first bound,
but the largest observed operating frame used only 23.2% of the tighter bound.
This evidence makes oversize a defensive counter/policy case, not a likely
steady-state event. It does not prove behavior under an unobserved pathological
scene or forced large IDR, so the guard must remain.

## Recommended sequence

1. [DONE] Establish repeatable baseline and injected-loss runs and preserve the
   artifact directories.
2. [PARTIAL] Add an offline trace/replay input around controller decisions,
   with frame size, arrival spacing, per-path delivery, RTT, and an explicit
   deadline.
3. Specify the oversized-block policy and absolute frame-deadline model before
   changing the wire or scheduler.
4. Characterize the SSC338Q's available QP/size actuation independently of the
   link, then choose the single encoder authority from measured behavior.
5. Implement the inner FEC/ARQ/deadline allocator first. Add bitrate/symbol-size
   control only after ablations show the inner loop and metrics are sound.
6. Return to RF for MCS, packet-duration/fade, correlated-loss, and uplink RTT
   experiments; Ethernet results cannot answer those questions.

### Replay foundation

`tools/jscc_replay.py` implements the first deterministic trace contract. A
finite Ethernet run emits `controller-trace.jsonl` and an independently
replayable `controller-decisions.jsonl`. Trace v1 deliberately uses excess
inter-arrival time over the run median because the two hosts do not yet share a
capture clock. The raw VFRM SDK PTS is retained only for correlation. It
calculates the exact frame allocation (`S`, `k`, target `m`, emitted
`m`, and the GF(256) capacity verdict) while leaving unobserved per-frame path
delivery and RTT as `null`. This makes the current evidence reproducible
without overstating what the UDP bench measures.
