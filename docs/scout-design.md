# Scout, claim, and channel-hold — implemented design

Status: **implemented through Pass 66.** Passes 58–63 (ANNOUNCE, claim/hold,
scout, discovery, key provenance, and quickconnect) landed in PR #29. Passes
64–66 are the two-adapter on-device hardening on the current feature branch.
`PROTOCOL.md` is normative; this document preserves the design narrative and
implementation history.

Goal: an easy, robust way to **find** a vehicle across channels and **CSA-jump**
it to an uncrowded channel — closer to analog-FPV simplicity than to an
associated link, with just enough guard to stop accidental cross-talk and
casual mid-flight takeover. Multiple vehicles may share the home channel,
separated by `net_id` + `originator`.

---

## 1. What already exists (reused, not rebuilt)

- **HEARTBEAT** — emitted 1 Hz while a node is otherwise quiet (§3.8). Suppressed
  by active DATA, so a video-active craft is discovered off its video instead.
- **`GET /api/v1/discovery`** — per-channel `{nodes[], streams[]}` from
  HEARTBEAT/DATA observations (§15.5). The scout drives this across channels.
- **`POST /api/v1/csa {mhz,class}`** — starts a §11 campaign. The "jump" already
  exists; claim reuses it.
- **`net_id` stamp + in-kernel BPF filter** — `stamp_net_id` and `filter_net_id`
  are already separate fields in the air backend (`air_radio`/`air_mon`); today
  both are seeded from one `node.net_id` at startup. Runtime decoupling is a
  local change, no wire change.
- **`retune()`** — per-adapter, the same path CSA uses.
- **Profile pin** — `POST /api/v1/link/profile {min,max}` (§9.7); the robust
  low-bitrate profile is the same operating point §9.8 falls back to on lost
  feedback.

The scout is therefore mostly orchestration over existing primitives plus one
new metric (channel occupancy) and one new wire packet (ANNOUNCE, §4).

---

## 2. Identity & separation

- **`net_id` is a `u8` → 256 values (0–255).** Convention: **0 = unassigned/
  default; 1–255 assignable** (255 separators — ample for 1–2 craft, fine for a
  small fleet). Auto-assign when omitted: `net_id = low byte of originator`, or a
  random 1–255; collisions are negligible at this scale and cost only a shared
  L2 filter, never correctness (`originator`+`session_id` still disambiguate).
- **`stamp` vs `filter` decouple at runtime** (the one change to existing net_id
  handling):
  - Craft: `filter = its net_id` (drops other craft at L2 before parse).
  - Ground while scouting: `filter = unset` (hears all net_ids).
  - Ground once locked: `stamp = filter = the craft's net_id` — **both**, because
    a craft filtering strictly only hears the ground's CSA/NACK if the ground
    stamps that net_id on its uplink. Ground learns the value from the craft's
    source address during scout.
- **`net_id` is not access control** (§13 unchanged) — it is an L2 partition/
  convenience only.

---

## 3. The reshaped flow (end to end)

1. **Boot** → home chan **161 / 5805 MHz @ 20 MHz** (or persisted channel, §7).
   Craft runs **low-bitrate video** (profile-pinned robust) + emits **ANNOUNCE**
   (§4) at ~1–2 Hz carrying `{claimed=false, psk=P}`, `P` auto-generated this
   session. `net_id` set (auto or config). Claim/bind state always starts clear.
2. **Ground scout** — one engine, two entry points (§6): sweep the allowlist,
   (a) measure per-channel occupancy, (b) discover craft (instant, off video
   DATA). Present a list, or quick-connect.
3. **Claim = CSA = jump, one event.** Ground reads the craft's announced `P` and
   `net_id` off the air, picks the least-crowded **allowlisted** channel X, fires
   `POST /api/v1/csa(X)` HMAC'd with `P`. Craft accepts → **binds this ground as
   its command source** → both jump to X together → craft unpins to normal
   bitrate. The §11.6 strand-proof ACK works because video is already flowing
   (this is why "always-on video" removes the old ignition state machine).
4. **Post-claim** — ground sets `stamp`+`filter` net_id to the craft's; both hold
   X.
5. **Mid-flight link loss** — **neither hops.** Craft §9.8-steps down to the
   robust bitrate but stays on X; ground holds X. Binding is sticky (§5).
6. **Reboot** — craft → home 161 (or persisted X); claim/bind state resets.

Detection note: while video flows the 1 Hz HEARTBEAT is suppressed, so the
"faster heartbeat for detection" idea is unnecessary — ANNOUNCE (§4) is the
periodic beacon that carries the pairing token, and discovery rides on video
DATA. Heartbeat stays 1 Hz as a pure keepalive.

---

## 4. ANNOUNCE packet (type `0xB`) — the pairing beacon

HEARTBEAT is fixed 11 bytes with no body (§3.8 ruling), so the pairing token
needs its own packet type. `0xB` is the next free type (11 of 16 used).

**Fixed 30-byte layout:**

| off | size | field | notes |
|---|---|---|---|
| 0 | 11 | *common* | §3.1 — sender = the craft; `destination` = 0 |
| 11 | 1 | `flags` | bit0 `claimed`; bit1 `psk_present`; others reserved 0 |
| 12 | 2 | `claimed_by` | `originator` of the binding ground, else `0` (UI/courtesy) |
| 14 | 16 | `psk` | the session pairing key when `psk_present=1`; all-zero otherwise |

- **Unauthenticated** (it is an advertisement, not a control action). A forged
  ANNOUNCE with a bogus `psk` only wastes one claim attempt — the craft rejects
  the resulting CSA on bad MAC. Matches the §13 threat posture.
- **Cadence** ~1–2 Hz. Emitted **always** (claimed or not) so a rebooted ground
  can re-learn `P` and re-claim after the binding releases (§5). `claimed`/
  `claimed_by` are advisory, for the ground UI.
- `psk_present=0` (secret mode, §5) sends 16 zero bytes — fixed size, leaks
  nothing.

---

## 5. Claim & command-source binding (the one genuinely new state)

The psk moves from "pre-shared secret" to "auto-generated session pairing token,
announced by default." **Be clear-eyed: an announced psk is readable by anyone in
RF range — it is a rendezvous/pairing token, not a secret.** The real defence
against casual mid-flight takeover is the **command-source binding**, not the
psk. HMAC stays always-on so the secret mode is a pure config flip.

- **`csa.psk` optional — the sole mode selector (Pass 61).** Absent → the node
  auto-generates a 16-byte `P` at boot (io/app RNG, same path as `session_id`;
  core stays clock/RNG-free) and announces it (`psk_present=1`). Present →
  operator secret: `csa.psk` is the config secret, **never** announced
  (`psk_present=0`), and the ground must have it pre-shared. This restores real
  cryptographic security; the binding rules below are identical.
- **Binding lifecycle:**
  - First accepted CSA (MAC-valid, allowlisted, nonce-fresh) **binds** its issuer
    as the craft's command source and enters COMMITTED (§11.5).
  - While bound, a CSA from any **other** issuer is rejected (existing §11.4
    "currently-latched command source" rule) — regardless of whether that issuer
    knows `P`.
  - **Binding is sticky through link loss.** It releases only after
    **`bind_release_s = 90 s`** with no command traffic from the bound issuer.
  - **Release changes no channel** — the craft stays on X and simply re-opens for
    claim (keeps announcing `P`), so a rebooted/returning ground re-claims *in
    place*. This is what resolves the orphan case (ground reboots while craft
    stays bound) without any short-timeout channel hopping.
- **Anti-replay unchanged in all modes:** nonce monotonic per `(originator,
  session)`, `target_chan ∈ channel_allowlist`, `csa_min_interval_s` rate-limit.
  These still hold even for the auto-psk default — an accepted CSA can never send
  a craft off its allowlist.
- The prior "psk=none, accept-anyone, skip-HMAC" idea is **withdrawn**: HMAC is
  always on; the zero-config convenience comes from announcing `P`, not from
  disabling authentication.

---

## 6. Scout engine (ground/rx node)

One sweep engine, two entry points; single- and dual-adapter both supported.

- **Sweep** the configured channel set (default = `channel_allowlist`), dwelling
  `scout.dwell_ms` per channel. A video-active craft is seen within a few hundred
  ms off DATA; for quick-connect, act on the **first** candidate rather than
  waiting for the §2 admission count (that admission gate is anti-flood for the
  latch picker, not needed when the operator is deliberately claiming).
- **Occupancy** per channel is reported as a record whose field set is a
  **superset aligned with the Realtek "Advanced Channel Scanning" (ACS) survey**
  (`/proc/net/rtl88x2eu/<if>/{acs,chan_info}` in the `libc0607/rtl88x2eu-20230815`
  fork: Quality/Availability/Utilization/WiFi-Util/Interference-Util/Noise dBm/
  BSS-count, raw CLM%/NHM%/noise/ITF). v1 fills only the **packet-derivable**
  fields from monitor RX over the dwell; the interference/total-utilization/
  quality fields stay reserved so a later hardware-ACS backend is a field-fill,
  not a reshape. Units are project-standard **per-mille** (ACS `%`→‰) and **dBm**.

  | field | v1 source | later (hardware ACS) |
  |---|---|---|
  | `wifi_util_permille` | decodable-frame airtime estimate over dwell | CLM Wi-Fi portion |
  | `util_permille` | = `wifi_util` (Wi-Fi only in v1) | total incl. non-Wi-Fi |
  | `interference_util_permille` | `null` (unmeasurable from packets) | CLM−Wi-Fi |
  | `noise_dbm` | RSSI-floor proxy (idle/min DBM_ANTSIGNAL) | NHM true noise floor |
  | `bss_count` | distinct BSSID/SA transmitters heard | ACS BSS count |
  | `quality_permille` / `availability_permille` | derived from the above | ACS direct |

  Rough and Wi-Fi-only in v1, enough to rank "emptiest allowlisted channel."
  **Exclude the candidate craft's own traffic** from these counts so its
  home-channel video doesn't make its own channel look occupied.
- **Two entry points:**
  - *List* — sweep, aggregate, return candidates + occupancy; operator chooses.
  - *Quick-connect* — sweep until the first (or a named) owned candidate, then
    claim it onto the emptiest allowlisted channel. Internally: set `stamp`+
    `filter` net_id → ensure the tx adapter is on the craft's current channel →
    `POST csa(X)` with the craft's `P` → confirm the §11.6 `CSA_ARMED` ACK.
- **Single-adapter** ground: scout is mode-exclusive — sweeping retunes the one
  uplink adapter, so any active link drops while scouting, and the CSA must be
  issued while parked on the craft's channel. **Two-adapter** ground: the
  `role:"tx"` uplink is the scout; diversity RX adapters remain on the resting
  channel and preserve an active link there (Pass 64).
- **Survey attribution:** only frames received by the scout/uplink adapter
  contribute occupancy and candidates. Resting diversity ears are excluded so
  they cannot smear one craft across every swept channel (Pass 65).
- **Candidate channel:** when buffered frames leak across a retune boundary, a
  craft's `chan` is the channel where it was heard most, not last (Pass 66).
- **Return/rollback:** sweep stop/completion returns all adapters to the resting
  channel. A failed claim also restores every adapter and the resting `net_id`
  stamp/filter; a successful claim moves every ear together (Passes 64–65).
- Post-claim, the ground does **not** auto-rescout on link loss (matches
  "hold until reboot"); re-scout is an explicit operator action.

---

## 7. Channel hold & persistence

- **COMMITTED is terminal until reboot** (§11.5 amendment): the mid-flight
  `rendezvous_timeout → REVERT → home` transition is **removed** for a committed
  link. The craft holds its channel through arbitrarily long ground outages
  (the >60 s asymmetric-TX case). The §9.8 adaptive step-down still applies (it
  changes bitrate, never channel).
- **The `verify_timeout` jump-backout stays** (§11.5): if the retune lands on a
  dead channel (no valid traffic within `verify_timeout_ms`, 150 ms) the craft
  reverts to `prev_chan` — this protects a *botched jump*, distinct from a
  *mid-flight* outage.
- **home_chan is demoted to "power-on default only."** It is no longer a
  mid-flight rendezvous, because the scout sweeps all channels — a craft holding
  any channel is still findable.
- **Persistence (optional, `persist_channel`, default `false`).** When true, the
  craft boots onto its last-committed channel instead of config home; suits the
  1–2-drone operator who wants stickiness across reboots. **Claim/bind state
  always resets on boot** regardless — persistence covers the channel only.

---

## 8. Config & control-plane additions

Config (§15.2), all §17-overridable seeds:

```json
"node":  { "originator": 17, "net_id": null },
"policy": { "csa": {
    "home_chan": 5805, "channel_allowlist": [5745, 5805, 5825],
    "bind_release_s": 90, "persist_channel": false
} },
"scout": { "dwell_ms": 300, "channels": null }   // null channels = allowlist
```

- `node.net_id: null` → auto-assign (§2). `csa.psk` absent → auto `P`, announced;
  present → operator secret, off the air (Pass 61: `csa.psk` presence is the whole
  selector). `csa.psk` stays redacted in every dump/stat (existing invariant).

Control plane (§15.5), acts on the ground/rx node:

| Method + path | Body | Effect |
|---|---|---|
| `POST /api/v1/scout/start` | `{ "channels":[...]?, "dwell_ms":?, "mode":"list"\|"quickconnect", "target":{"originator":?}? }` | begin a sweep |
| `GET /api/v1/scout/results` | — | `{ scanning, current_chan, channels:[{chan,evidence_valid,occupancy:{decoded_airtime_permille,ranking_score_permille,interference_score_permille,duty_cycle_known,...legacy aliases}}], candidates:[{originator,net_id,session,claimed,claimed_by,chan,frames,resolved,psk_known}], candidate_sightings:[{originator,net_id,session,chan,frames,resolved}] }` — `candidates` is deduplicated/resolved and actionable; `candidate_sightings` is raw diagnostic evidence; FA/CCA score is never presented as measured duty cycle. |
| `POST /api/v1/scout/stop` | `{}` | end the sweep |
| `POST /api/v1/scout/quickconnect` | `{ "originator":N, "target_chan":?? }` | claim a discovered craft onto the given (or emptiest allowlisted) channel |

The current `scout/results` candidate shape reports `psk_known`, not the cached
token itself. The announced token is nevertheless public and may be surfaced
(Pass 63); the operator-provisioned `csa.psk` remains secret and redacted.

---

## 9. Original open decisions — RESOLVED (operator ruling 2026-07-20)

1. **ANNOUNCE = type `0xB`, fixed 30 bytes** with `flags` + `claimed_by` +
   16-byte `psk` — **approved as specified.**
2. **16-byte auto `P`, generated in io/app** (core stays RNG-free) — **approved.**
3. **Cadence 1–2 Hz, emitted always** (claimed included) — **approved.**
4. **Occupancy folds in all metrics** and mirrors the Realtek ACS survey field
   set (§6) so the future hardware "Advanced Channel Scanning" backend
   (`libc0607/rtl88x2eu-20230815`) is a field-fill; v1 does a packet occupancy
   scan only — **approved.**
5. **Config key names** (`bind_release_s`, `persist_channel`, `scout.dwell_ms`)
   — **approved as spelled.** (`psk_announce` was later removed — Pass 61 makes
   `csa.psk` presence the sole mode selector.)

---

## 10. Pass history

- **Pass 58 — Session pairing token + ANNOUNCE packet.** Adds packet type `0xB`
  (§3, §4 here), the auto-generated announced `P` default, and HMAC-always-on.
  Amends §3.1 (type registry), §11.4 (key provenance; announced vs secret), §15.2
  (config). (Mode selection later simplified to `csa.psk`-presence-only — Pass 61.)
- **Pass 59 — CSA follower holds until reboot; command-source binding
  lifecycle.** Removes the mid-flight `rendezvous_timeout → home` revert, keeps
  the `verify_timeout` jump-backout, defines sticky binding with a 90 s
  no-channel-change release, and demotes `home_chan` to power-on default. Amends
  §11.4 (binding release) and §11.5 (state machine), §15.2 (`bind_release_s`,
  `persist_channel`).
- **Pass 60 — Ground scout, channel occupancy, and channel persistence.** The
  sweep engine, occupancy metric, and control-plane endpoints. Amends §15.2
  (`scout`) and §15.5 (endpoints); io/app feature, no additional wire change.
- **Pass 61 — Key provenance from `csa.psk` presence.** Removed the inert
  `psk_announce` toggle: configured `csa.psk` selects secret mode; absence
  selects the announced token.
- **Pass 62 — ANNOUNCE subsumes HEARTBEAT.** ANNOUNCE is a discovery presence
  source and resets the same quiet interval; announcing craft need not emit a
  redundant HEARTBEAT.
- **Pass 63 — Announced token is public.** Ground caches it for claims and may
  surface it; only the configured operator secret remains redacted.
- **Pass 64 — Multi-adapter scout ownership.** The uplink roams during a sweep;
  claim moves every adapter together.
- **Pass 65 — Survey isolation and clean rollback.** Only scout-adapter frames
  feed the survey; sweep completion and failed claims restore all ears and the
  resting `net_id` state.
- **Pass 66 — Evidence-weighted candidate channel.** A craft's claim channel is
  where it was heard most, preventing adjacent-channel settling leakage from
  winning by scan order.

## 11. Implementation status

1. Passes 58–66 are recorded in `docs/review-log.md`; every spec amendment
   preceded its implementation commit.
2. Core ANNOUNCE codec and CSA binding lifecycle are implemented and unit-tested
   with injected time.
3. Runtime token generation, stamp/filter decoupling, channel hold, and optional
   persistence are implemented in io/app.
4. Scout sweep, occupancy, list/quickconnect endpoints, multi-adapter retune,
   rollback, and heard-most channel selection are implemented.
5. The native merge gate is 43/43 and the SSC338Q cross-build is clean. Passes
   64–66 came from two-adapter on-device verification; range-sensitive gate-4
   and real-TSF CSA timing remain separate hardware follow-ups.
