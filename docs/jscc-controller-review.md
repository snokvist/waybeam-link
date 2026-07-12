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

Trace v1 also accepts the optional `waybeam-packet-events-v1` bench stream.
The UDP backend observes submitted, accepted, filtered, and synthetic-drop
events behind a fixed cap; it is disabled unless `WBLINK_PACKET_TRACE` is set.
The event builder groups existing wire packets by block and retains per-path
delivery, NACKs, and retransmissions. Replay provides seeded burst,
incremental, sparse-periodic, and high-frequency loss with independent or
fully correlated diversity plus FEC/ARQ/deadline-discard ablations. This is
tooling only: no DATA field, protocol rule, or production stats field changed.

### Initial packet-event matrix

A complete local frame-SHM run captured 302 blocks from a 15 Mbit/s x265 snow
stream through one UDP broadcast and two shared-port listeners. Both traces
ended normally with zero dropped trace events; all recorded/no-loss blocks took
the fast path. The standard matrix then replaced delivery with deterministic
15% high-frequency loss, 12/100-packet bursts, ramping loss, or sparse periodic
loss. Selected results (`fast / FEC / failed`) were:

| scenario | fixed FEC | no FEC |
|---|---:|---:|
| 12/100 burst, independent paths | 302 / 0 / 0 | 302 / 0 / 0 |
| 12/100 burst, correlated paths | 135 / 21 / 146 | 135 / 0 / 167 |
| incremental, independent paths | 226 / 76 / 0 | 226 / 0 / 76 |
| sparse periodic, correlated paths | 170 / 129 / 3 | 170 / 0 / 132 |
| 15% high-frequency, independent paths | 109 / 190 / 3 | 109 / 0 / 193 |
| 15% high-frequency, correlated paths | 0 / 72 / 230 | 0 / 0 / 302 |

At the provisional 16 ms deadline and 4 ms RTT, ARQ recovered none of the
synthetic ARQ-class IDRs: their original packet train had already consumed the
available recovery window. This supports the architecture's RTT/deadline gate;
it is not evidence that ARQ is useless for the real GDR stream.

The snow producer also dropped one periodic IDR above the 512 KB SHM slot at 15
Mbit/s (and three at 25 Mbit/s in an earlier incomplete stress trace). This is
kept separate from the real `size=auto` GDR envelope, where the measured maximum
was 74,789 B. Normal benches fail on synthetic oversize; an explicit
`ALLOW_PRODUCER_OVERSIZE=1` exists only to retain otherwise-valid stress runs.

### Initial ARQ deadline gate

The same real TX/RX scheduler path was exercised at 8 Mbit/s with FEC disabled,
one UDP-broadcast listener, and deterministic packet loss. The synthetic encoder
emits one ARQ-class IDR every 30 frames; ordinary P-frames remain unprotected.

| loss | deadline | NACK | resend | recovered packets | delivered frames | delivered IDR |
|---:|---:|---:|---:|---:|---:|---:|
| 10% | 80 ms | 172 | 110 | 97 | 31/303 | 1/11 |
| 2% | 80 ms | 44 | 14 | 13 | 183/303 | 9/11 |
| 2% | 16 ms | 17 | 12 | 10 | 197/303 | 8/11 |
| 2% | 8 ms | 15 | 11 | 0 | 188/303 | 0/11 |

The initial recovered-packet RTT was at most 9 ms. Profiling found two
application delays rather than Ethernet propagation: TX could sleep on local
ingress while a return was ready, and paced UDP queued an authorized resend
behind the current live-frame burst. Pass 26 added air-readiness wakeups to all
three backends and a bounded resend-priority lane after the existing scheduler
gates. Repeating the same 2%/16 ms synthetic run recovered 15 packets, all in
the <=1 ms bucket (maximum 1 ms).

A real SSC338Q-to-x86 ARQ-only run used the stable auto/25 Mbit/s/60 fps encoder,
15% independent loss per each of two virtual listeners, and repeated forced GDR
recovery frames. It measured 19 recoveries: 16 in <=1 ms and three in <=2 ms;
P90 <=2 ms, maximum 2 ms, with no ARQ deadline drops. This establishes that the
application path can fit a 16 ms Ethernet deadline. It does not set the RF
deadline: monitor/devourer testing must still include airtime, contention,
quiet-gap scheduling, and return loss before the runtime controller consumes an
RTT percentile.

### Causal loss-estimator shadow

Packet-event replay now includes a tooling-only trailing-window empirical
repair-demand quantile. A block is predicted strictly from previously completed
blocks and contributes its observation only after its outcome is fixed.
Adaptive replay limits itself to repair symbols present in the captured trace,
so it cannot claim protection that was never transmitted. Summaries report
selected versus available parity, estimator underprediction, and observations
that are censored because the captured fixed parity could not recover them.
Repair demand is the first transmitted repair index that supplies enough
received equations, so lost repair packets count as protection demand. Demand
is normalized by block `k` before estimation and converted back to symbols for
each new block; this avoids applying a large frame's raw loss count to a small
frame or vice versa.

On the 302-block synthetic corpus with 15% high-frequency independent loss,
the conservative 120-block maximum reduced selected parity by about 25% but
still produced three more deadline failures than fixed FEC. This rejects
immediate adaptive actuation: parity saving alone is not the objective.

The initial source-loss estimator runs in shadow at frame-SHM RX using a 120-block P95,
20-sample threshold, and zero cold start. It changes no transmitted parity or
ARQ decision. A controlled real-encoder Ethernet run injected 10% independent
loss into each of two broadcast listeners. Across 1,072 finalized frames,
fixed FEC delivered 747 frames fast and recovered 325, with zero unrecoverable,
deadline, kernel, or SHM-full drops. Shadow selected 1,548 hypothetical parity
symbols and underpredicted 45 blocks (4.2%); the latest converged prediction was
two lost source symbols. The underprediction tail is visible in the live
dashboard and is why §14.1 fixed rates remain authoritative.

The normal zero-loss auto/25 Mbit/s/60 fps bench was restored after this run.
Monitor-mode and Devourer/RF validation remains deliberately deferred.

Protection-aware replay tested a 120-block maximum and a conservative 10%
cold-start rate against the same matrix. In 15% high-frequency independent
loss it matched fixed FEC's 190 recoveries and three failures while selecting
13.1% less parity. Under incremental independent loss it selected 46.2% less
parity but introduced one additional failure; under correlated burst loss it
matched the already-limited fixed result and saved no parity. The remaining
incremental-loss miss means this candidate is still shadow-only. Runtime
telemetry must be upgraded from raw missing sources to repair demand before the
candidate can be judged on the real encoder stream.

After that upgrade, a second 10%-per-listener real-encoder run observed 1,992
frames: 1,361 fast and 631 fixed-FEC recoveries, with zero unrecoverable,
deadline, kernel, or SHM drops. Exact TX counters measured 4.10 fixed repair
symbols/frame; protection shadow predicted 3.37/frame, a 17.9% reduction, but
underpredicted one block. Replay safety-margin ablation found that one extra
symbol did not remove the incremental-loss miss; two did, retaining 9.1% parity
saving under steady high-frequency independent loss and 18.8% under incremental
loss because selection remained capped by fixed available parity. A runtime
actuator must retain that fixed-policy cap. The live estimator is still
non-enforcing, and an in-process loss ramp is still needed because restarting
the receiver resets its causal window.
