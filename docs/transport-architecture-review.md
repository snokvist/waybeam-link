# Transport architecture and verification review

Date: 2026-07-12. Branch: `audit/transport-parity-bench`.

## Scope and verdict

The application has two separate interface planes:

1. Local stream bindings: UDP datagrams or whole encoded `frame-shm` frames.
2. Air backends: UDP simulation, Linux kernel-monitor injection, or devourer
   direct USB radio.

The core framing, diversity, FEC, ARQ, and frame-SHM/UDP-air data path is fit
for continued bench and field development. The clean real-video chain passed at
1, 4, and 8 Mbit/s, and a 10% independent-loss run recovered and decoded its
target frame count. It is not yet safe to call all three air backends feature or
stats equivalent. Kernel-monitor CSA, restart handling, discovery/heartbeat,
and several observability gaps need resolution before unattended deployment.

## Findings

### Critical

1. **Kernel-monitor CSA reported success without retuning (fixed fail-closed).**
   `MonAir::retune()` logs that retuning is deferred and returns `true`.
   `AirBackend::retune_all()` ignores that semantic distinction and the CSA
   state machine proceeds. A control request can therefore make protocol state
   say COMMITTED while the interface remains on its original channel. Until an
   nl80211 retune is implemented, CSA must be rejected/disabled for this
   backend rather than acknowledged. This branch now rejects local CSA control
   and ignores received campaigns on kernel-monitor; real retune remains absent.

2. **Frame-SHM producer restart strands an attached consumer.**
   `FrameShmRing::create()` unlinks and recreates the name. Existing mappings
   remain attached to the old object. TX retries only while its ring pointer is
   null, so a venc restart after initial attachment is never detected. The same
   issue affects an egress consumer when the RX process recreates its ring.
   The header has an `epoch` field but it is always initialized to zero and
   cannot identify replacement objects. Recovery needs an epoch/producer
   liveness contract or periodic name/inode revalidation and reattachment.

### High

3. **The normative HEARTBEAT has no application implementation.**
   The codec and vectors exist, but no mode schedules or handles HEARTBEAT.
   Quiet TX nodes send nothing. This weakens rendezvous and makes passive
   discovery dependent on active DATA/return traffic, contrary to the protocol
   presence mechanism.

4. **`loss_prediversity_milli` is permanently zero at runtime.**
   The schema, dashboard, and protocol describe it as the raw diversity/correlation
   gauge, but `RxCore::fill_stats()` only computes post-diversity loss. The
   existing `gate2_rho.py` reconstructs per-adapter loss from deltas, so bench
   analysis works, but the advertised live metric and `/health` input do not.
   A normative estimator is required before implementation because retransmits,
   adapter reorder, and control packets make a naive adapter-RX ratio ambiguous.

5. **Frame-SHM egress backpressure is invisible in node stats.**
   `FrameShmRing` counts full, oversize, and bad-slot outcomes, but the RX
   delivery callback ignores `write_frame()` failure and none of these counters
   enter section 15.3 stats. A healthy air link can therefore show successful
   frame reassembly while the decoder loses frames at a full SHM ring.

6. **UDP kernel queue overflow is not counted.**
   UDP-air `drop` counts configured synthetic loss only. Kernel receive-queue
   overflow silently appears as air loss and cannot be reconciled against
   ground truth. This branch raises `SO_RCVBUF` to tolerate frame bursts, but a
   fit-for-purpose simulator should enable/read `SO_RXQ_OVFL` and report it
   separately from injected loss.

### Medium

7. **Discovery exists internally but has no scan/list API.**
   Every DATA header carries originator, session, stream ID, and stream type.
   An output config may omit originator and the RX will admission-latch the
   first matching `stream_type`, so automatic hook-on is possible today.
   There is no endpoint that lists candidate streams, sessions, activity, or
   type before selecting one, and a taken local output cannot switch until idle
   teardown. HEARTBEAT cannot advertise stream types because its specified body
   is empty.

8. **ARQ duplex topology is manual.**
   RX correctly emits NACK/LINK_REPORT through the same air backend. Radio and
   kernel-monitor require exactly one `role:"tx"` adapter even on a ground RX
   node; UDP requires reciprocal `tx`/`rx` endpoints. Sample configs explain
   this, but config does not derive or validate a matched return pair. A topology
   generator should expand a stream declaration into downlink plus return path,
   assign the preferred originator, and validate that ARQ-enabled streams have
   a usable injector.

9. **Startup admission can discard the first frame.**
   Admission intentionally waits for three matching packets, then uses the
   third packet as the stream floor. With frame-SHM this can leave the first
   frame below `k`; it is later superseded. The real-video harness uses explicit
   warm-up frames. This is safe for corruption but should be documented as
   startup behavior and aligned with decoder IDR acquisition expectations.

10. **A first forged symbol can deny one frame.**
    The reassembler now rejects later conflicting metadata, but the first
    unauthenticated symbol necessarily defines a block's `k`/size metadata.
    An attacker can deny a frame, consistent with the protocol's unauthenticated
    data-path threat model; it can no longer cause conflicting legitimate
    symbols to produce corrupt output.

## Packet and frame lifetime

### UDP stream binding

`UdpIngress` receives one RTP/telemetry datagram. `Framer` validates size,
classifies importance, assigns block/sequence/profile fields, and encodes one
DATA packet. TX injects it and copies it into `ResendRing`. `ResendScheduler`
may later copy and mark it RETRANSMIT. The air backend owns/copies only for the
duration required by its send or bounded RX queue. RX decodes, admission-latches,
deduplicates across adapters, holds by sequence, declares gaps, and synchronously
delivers the original datagram to `UdpEgress`.

### Frame-SHM binding

The encoder publishes `[VencFrameMeta][Annex-B AU]` into the SPSC ring. TX copies
one slot into its reusable buffer; `FrameFramer` atomically fragments the frame,
emits source then repair symbols, and copies each encoded packet into the resend
ring. RX performs packet admission/dedup/order first, then `FrameReassembler`
copies symbol payloads by block. Completion is all-source concatenation or
Cauchy-RS decode into scratch storage. The callback synchronously copies the
whole frame into an egress SHM slot. Partial frames are never published.

Ownership is sound within a running process: callback buffers are borrowed only
for the call and every asynchronous boundary copies. The unresolved lifetime
problem is cross-process producer replacement, not in-process buffer ownership.

## Air-backend parity

| Capability | UDP-air | Kernel-monitor | Devourer radio |
|---|---|---|---|
| DATA/control duplex | Yes, explicit endpoints | Yes, designated TX interface | Yes, designated TX USB adapter |
| Multi-adapter diversity | Multiple listen sockets | One RX thread/interface | One RX thread/USB adapter |
| RSSI / TSF | Synthetic/zero | Radiotap when supplied | Chip RX metadata |
| MCS actuation | No-op | Per-packet radiotap | Device `SetTxMode` |
| Power actuation | No-op | Logged/no-op | Device TX power offset |
| Return unicast/HW ACK | No | Broadcast fallback | Supported with SA latch |
| TX report/wedge detection | No | No | CCX reports |
| CSA retune | Intent only by approved scope | Not implemented; now rejected | Implemented |
| Synthetic uniform loss | Yes | Yes | Yes |
| RX/TX/drop adapter stats | Yes after this review | Yes | Yes after this review |

## Verification results

- Host ASan/UBSan suite: 31/31 pass. LeakSanitizer is disabled only because the
  command environment traces children; assertions and ASan/UBSan remain active.
- SSC338Q ARMv7 cross-build: pass.
- Real H.265 clean chain, 93 generated/90 measured frames per point:
  - 1 Mbit/s: 508,322 encoded bytes; decode/metadata/PTS clean.
  - 4 Mbit/s: 1,833,156 encoded bytes; decode/metadata/PTS clean.
  - 8 Mbit/s: 3,851,315 encoded bytes; decode/metadata/PTS clean.
- 10% independent loss per UDP adapter at 4 Mbit/s: 90 decoded frames,
  6 FEC-recovered frames, 10 NACK batches, 6 retransmits, 10 permille measured
  post-diversity loss, zero malformed/decode failures.
- A high-detail 10 Mbit/s run produced one frame above the fixed 512 KiB SHM
  slot. It was correctly rejected as `oversize_drop`; this is a real encoder/
  ring sizing boundary and must be included in bitrate/GOP tuning.
- Deterministic core-chain cases now cover independent 10% and 35%, correlated
  Gilbert-Elliott bursts, non-ARQ loss, a 95% deep fade and recovery, periodic
  2% loss, incremental 1/5/15/30% loss, and 50% return-path loss. ARQ remained
  bounded by scheduler caps and recovery resumed after fades.

## UDP broadcast/sniffer follow-up

Linux loopback supports the desired model: a probe to `127.255.255.255:PORT`
with `SO_BROADCAST` was received by two sockets bound to `0.0.0.0:PORT` with
`SO_REUSEADDR`. This would better model one broadcast transmission observed by
multiple receivers than the current TX target fanout.

Recommended implementation is a separate UDP-air broadcast mode, not a change
to normal point-to-point UDP:

1. One TX datagram to a configured broadcast group/port.
2. Receivers bind a shared port and filter by normal waybeam wire decode,
   originator/session/type, exactly as RF receive does.
3. Use multicast rather than subnet broadcast when host/network portability is
   important; loopback broadcast is suitable for the Linux CI bench.
4. Add `SO_RXQ_OVFL`, sender identity, and duplicate/self-filter tests.

## Recommended sequence

1. Define and implement frame-SHM producer epoch/reconnect behavior.
2. Implement HEARTBEAT scheduling/handling and a read-only discovery endpoint.
3. Specify the pre-diversity estimator and expose SHM/kernel-overflow counters.
4. Add a config topology expander for paired ARQ return paths.
5. Add optional UDP broadcast/sniffer mode as a dedicated simulation backend.
