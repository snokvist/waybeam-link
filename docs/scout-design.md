# Scout, claim, and channel-hold — design (pre-implementation)

Status: **awaiting operator sign-off.** No code, no `PROTOCOL.md` edit, and no
`review-log.md` Pass entry is committed until the wire format and the open
decisions in §9 are approved. This doc is the proposal; the law (`CLAUDE.md`)
requires spec amendments commit first, each as a numbered Pass, before code.

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

## 4. ANNOUNCE packet (proposed type `0xB`) — the pairing beacon

HEARTBEAT is fixed 11 bytes with no body (§3.8 ruling), so the pairing token
needs its own packet type. `0xB` is the next free type (11 of 16 used).

**Proposed fixed 30-byte layout:**

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

- **`csa.psk` optional.** Absent + `psk_announce=true` (default) → the node
  auto-generates a 16-byte `P` at boot (io/app RNG, same path as `session_id`;
  core stays clock/RNG-free) and announces it. Present → operator secret.
- **`psk_announce` (default `true`).** `false` = advanced/secure: `P` is the
  config secret, **never** announced (`psk_present=0`), and the ground must have
  it pre-shared. This restores real cryptographic security; the binding rules
  below are identical.
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
- **Occupancy** per channel = a simple busy metric from monitor RX (all frames,
  not just waybeam): frames/s and/or RSSI floor over the dwell, normalized to a
  `busy_permille`. Rough, WiFi-only, not a spectrum analyzer — enough to rank
  "emptiest allowlisted channel." **Exclude the candidate craft's own traffic**
  from the busy count so its home-channel video doesn't make its own channel look
  occupied.
- **Two entry points:**
  - *List* — sweep, aggregate, return candidates + occupancy; operator chooses.
  - *Quick-connect* — sweep until the first (or a named) owned candidate, then
    claim it onto the emptiest allowlisted channel. Internally: set `stamp`+
    `filter` net_id → ensure the tx adapter is on the craft's current channel →
    `POST csa(X)` with the craft's `P` → confirm the §11.6 `CSA_ARMED` ACK.
- **Single-adapter** ground: scout is mode-exclusive — sweeping retunes the one
  tx adapter, so any active link drops while scouting, and the CSA must be issued
  while parked on the craft's channel. **Two-adapter** ground: dedicate a scout
  adapter (the §15.2 "scout on a different channel" note already anticipates
  this) and keep the link on the other.
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
"node":  { "originator": 17, "net_id": null, "psk_announce": true },
"policy": { "csa": {
    "home_chan": 5805, "channel_allowlist": [5745, 5805, 5825],
    "bind_release_s": 90, "persist_channel": false
} },
"scout": { "dwell_ms": 300, "channels": null }   // null channels = allowlist
```

- `node.net_id: null` → auto-assign (§2). `csa.psk` absent → auto `P` when
  `psk_announce`. `csa.psk` present → secret; set `psk_announce:false` to keep it
  off the air. `csa.psk` stays redacted in every dump/stat (existing invariant).

Control plane (§15.5), acts on the ground/rx node:

| Method + path | Body | Effect |
|---|---|---|
| `POST /api/v1/scout/start` | `{ "channels":[...]?, "dwell_ms":?, "mode":"list"\|"quickconnect", "target":{"originator":?}? }` | begin a sweep |
| `GET /api/v1/scout/results` | — | `{ scanning, current_chan, channels:[{chan,busy_permille,nodes:[…]}], candidates:[{originator,net_id,session,claimed,claimed_by,chan,psk_known}] }` |
| `POST /api/v1/scout/stop` | `{}` | end the sweep |
| `POST /api/v1/scout/quickconnect` | `{ "originator":N, "target_chan":?? }` | claim a discovered craft onto the given (or emptiest allowlisted) channel |

`psk` is never echoed by `scout/results` — `psk_known` is a bool.

---

## 9. Open decisions needing an explicit yes/no (before any PROTOCOL.md edit)

1. **ANNOUNCE = new packet type `0xB`, fixed 30 bytes** with the §4 field set
   (`flags`, `claimed_by`, 16-byte `psk`)? Or trim `claimed_by` / go
   variable-length?
2. **16-byte auto `P`** generated at boot in io/app (core stays RNG-free)? OK?
3. **ANNOUNCE cadence 1–2 Hz, emitted always** (claimed included) so re-claim in
   place works? OK?
4. **Occupancy metric = normalized all-frame count over the dwell** (rough,
   WiFi-only)? Good enough, or do you want RSSI-floor folded in?
5. Config key names above (`psk_announce`, `bind_release_s`, `persist_channel`,
   `scout.dwell_ms`) — accept as spelled?

---

## 10. Proposed Pass entries (draft — appended to review-log.md on approval)

- **Pass 58 — Session pairing token + ANNOUNCE packet.** Adds packet type `0xB`
  (§3, §4 here), the auto-generated announced `P` default, `psk_announce`, and
  HMAC-always-on. Amends §3.1 (type registry), §11.4 (key provenance; announced
  vs secret), §15.2 (config).
- **Pass 59 — CSA follower holds until reboot; command-source binding
  lifecycle.** Removes the mid-flight `rendezvous_timeout → home` revert, keeps
  the `verify_timeout` jump-backout, defines sticky binding with a 90 s
  no-channel-change release, and demotes `home_chan` to power-on default. Amends
  §11.4 (binding release) and §11.5 (state machine), §15.2 (`bind_release_s`,
  `persist_channel`).
- **Pass 60 — Ground scout, channel occupancy, and channel persistence.** The
  sweep engine, occupancy metric, and control-plane endpoints. Amends §15.2
  (`scout`) and §15.5 (endpoints); io/app feature, no additional wire change.

## 11. Implementation order (deferred until §9 sign-off)

1. Pass 58/59/60 spec commits (one each), then review-log entries.
2. core: ANNOUNCE codec + CSA binding-lifecycle state (unit-tested, clock
   injected).
3. io/app: auto-`P` + net_id auto, runtime stamp/filter decouple, §11.5 hold,
   optional persistence.
4. io/app: scout engine + occupancy + control-plane endpoints.
5. `dev` gate green (ASan+UBSan), `ssc338q` compile clean, then bench.
