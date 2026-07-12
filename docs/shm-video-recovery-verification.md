# SHM video recovery verification — 2026-07-12

## Root cause

The VFRM producer and transport remain live when Radeon-VRX disables and
re-enables SHM ingress. Radeon continues to advance the shared `read_idx`, so
waybeam-link has no VFRM v1 signal that the decoder generation changed.

An IDR request is functional. A read-only probe of `/venc_frame_out` observed
the request response `{"idr":true}` followed by:

| offset after request | meta flags | bytes | Annex-B NAL types |
|---:|---:|---:|---|
| 0–1 | `1` | 4.6–6.4 KiB | `32,33,34,19` (VPS/SPS/PPS/IDR_W_RADL) |

The IDR flag and IRAP NAL survive venc SHM ingress, frame fragmentation, UDP
transport, reassembly, and ground SHM egress. A 180-AU sample contained NAL
types 1/19/32/33/34, one requested type-19 AU, one matching IDR metadata flag,
and no PTS reversal.

The checkbox-specific failure is in Radeon-VRX's settings lifecycle. Applying
`shm_enabled` destroys the complete `UvViewer`. The replacement relay
registers the SHM source asynchronously but does not restore the old selected
source identity; it can therefore drain and account the ring while
`selected_index == -1`, with no route to the SHM appsrc. A perf uprobe on
`dec_src_probe` counted zero decoded frames for three seconds after re-enable,
including after a valid requested IDR reached the ring. Selecting the SHM source
and requesting recovery produced 294 decoded frames in the next three seconds
(about 98 fps), without restarting waybeam-link.

## Waybeam-link changes

- `RECOVERY_REQUEST` is an exact-session, exact-stream return packet.
- Ground `POST /api/v1/video/recover` emits it for the latched RTP stream.
- The matched TX rate-limits requests to one per second and invokes venc
  `/request/idr`.
- `tools/jscc_ethernet_bench.sh recover-video` drives the bench endpoint.
- Producer teardown only unlinks its own SHM inode, so destruction of an old
  orphan generation cannot unlink a newer replacement.
- SHM publication wakes the shared futex after release-publishing
  `init_complete=1`.

## Live verification

- Five Radeon pipeline rebuilds: same Radeon PID, 41 threads and 29 FDs before
  and after; each recovery produced VPS/SPS/PPS/type-19 with `flags=1` on the
  unchanged SHM inode.
- Three complete waybeam-link restarts: three distinct ground ring inodes and
  epochs; every header had `init_complete=1`; Radeon reattached without a
  process restart and kept `read_idx == write_idx`.
- During the three measured restart windows: zero SHM full drops, oversize
  drops, or bad slots.
- Clean native build and all 33 tests passed. Release and SSC338Q builds passed.

## Required Radeon-VRX integration

Radeon-VRX must preserve/reselect the prior source by stable identity
(`kind=SHM`, normalized ring name) after settings recreate the viewer. Once
the replacement SHM pipeline reaches PLAYING and appsrc is accepting buffers,
it must call the local ground recovery endpoint. Requesting before source
selection wastes the IDR because the ring is still drained while appsrc push is
disabled. This consumer change was not made here because the supplied
Radeon-VRX sources are read-only references for this task.
