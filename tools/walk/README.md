# Walk-test capture and analysis

Used for the 2026-08-21 range walk (`docs/findings.md`), which produced the
§9.4 probe's first real-link data and the §6.3b Phase E numbers.

- `S98walklog` — craft-side init script. Polls `127.0.0.1:8091/api/v1/stats` at
  1 Hz to the SD card at `/mnt/mmcblk0p1/walklog/`. It exists because
  **`promote_blocked_probe` is craft-only (§15.3)** — the grounds receive the
  craft's §3.15 selector word but not the veto counter, and the craft is
  unreachable in the field. Sorts after `S97waybeam-hub`, waits for the control
  server rather than racing it, and never writes to the 5.7 MB overlay.
- `analyze_walk.py <ground.ndjson> [craft.ndjson]` — buckets `probe_per` by RSSI
  and candidate MCS, prints the profile trajectory, the §6.3b totals, and the
  craft-only veto counter.

Two traps the first run paid for:

- **The craft has no RTC.** On a battery boot its wall clock resets by weeks, so
  the log filename and `t_wall` are wrong. `t_up` (uptime) is the reliable axis;
  correlate with the ground record via link events (a counters reset marks a
  craft session change).
- **RSSI above −20 dBm is near-field compressed** and can read positive at bench
  range. The analyzer reports those samples separately and excludes them from
  the range bins, because a saturated front end inverts the evidence.

Set both ends to `power_offset_qdb: 0` before a walk and restore the bench
floors afterwards (`deploy/README.md`), or every subsequent bench RSSI lies.
