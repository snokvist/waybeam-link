# Diagram brief — README architecture figures

Source material for the architecture figures in `README.md`. Hand the whole of
this file to an image/diagram generator; it is written to be self-contained so
the generator does not have to infer any system facts.

**What shipped (2026-08-17):** one composite asset,
`docs/images/architecture.png`, laying the five figures out as labelled panels
— 1 and 2 full width, then 3/4/5 across a single row. It replaced a per-figure
SVG attempt that came back poorly. Regenerate from this brief and keep the two
in sync when either changes.

The style rules and system facts below are the load-bearing part and apply to
any regeneration, composite or not.

---

## Global style constraints (apply to every figure)

- **Format:** SVG preferred (crisp on GitHub at any zoom). PNG at 2x if SVG is
  not available. Target width 1400–1600 px; must stay readable when GitHub
  scales it down to ~850 px.
- **Theme:** must be legible on **both** GitHub light and dark backgrounds. Use
  a solid light background rather than transparent, or supply light and dark
  variants.
- **Palette:** restrained and technical, 4–5 colours maximum.
  - Vehicle / airborne side — one warm accent (amber or orange)
  - Ground side — one cool accent (blue or teal)
  - The RF hop — a distinct neutral (violet or slate)
  - Everything structural — greys
  - Red reserved exclusively for loss, failure and back-out paths
- **Typography:** one clean sans-serif (Inter, Source Sans, Helvetica). No
  decorative fonts. Labels ≥ 14 px at the stated canvas width.
- **Line semantics — keep consistent across all five figures:**
  - Solid arrow = video / bulk data
  - Dashed arrow = control, reports, requests
  - Double or thick line = the RF hop
  - Dotted = optional or bench-only path
- **No 3-D, no drop shadows, no gradients, no clip-art drones or laptops.**
  Flat boxes, clean arrows, generous whitespace. Aim for the visual register of
  a good protocol RFC or an O'Reilly systems diagram, not a marketing deck.
- **Every arrow is labelled.** An unlabelled arrow is a bug.
- **Do not invent components, port numbers, or product names** beyond what this
  brief states.

---

## System facts the figures must reflect

These are load-bearing. Getting them wrong makes the diagram misleading.

1. **The vehicle has exactly ONE radio.** Never draw two. This is a fixed
   physical constraint, and most of the design follows from it.
2. **The ground has one to three radios**, all in a single process, merged into
   one receive stream. Diversity is a **ground-only** capability.
3. **Exactly one ground adapter transmits.** The others are receive-only ears.
4. The link is **broadcast** — no association, no handshake, no pairing
   required to *watch*. Extra receivers are free.
5. **No kernel WiFi driver is involved.** A userspace USB driver (devourer)
   owns the adapter directly over libusb.
6. Video moves as **whole encoded frames through shared memory**, not as a
   network socket, on both the vehicle and the ground host.
7. **The receiver measures, the vehicle decides.** All adaptive control
   decisions are made on the vehicle.
8. The vehicle can only *hear* the ground while it is not transmitting, so the
   return path is narrow, opportunistic and best-effort.

---

## Figure 1 — System overview  *(README "Architecture")*

**The one figure that has to work alone.** A reader who sees only this should
understand the whole system.

Left half, labelled **VEHICLE**, warm accent, drawn inside a single boundary:

- `Camera + Encoder` → arrow labelled *shared memory ring (whole frames)* →
- `waybeam-link (TX)` — show three stacked internal functions:
  *frame framer*, *FEC (Reed–Solomon)*, *adaptive selector*
- → arrow labelled *inject* → `1 × USB WiFi radio`
- A small side annotation on the boundary: **"one radio — timeshares TX video
  and RX return"**

Centre: the **RF hop**, drawn as a wide gap crossed by:

- A thick solid arrow left→right labelled **video + telemetry (broadcast)**
- A thin dashed arrow right→left labelled **return: repair requests, link
  reports, control** — visibly thinner, and annotated *"best-effort, fits in
  the vehicle's quiet gaps"*

Right half, labelled **GROUND STATION**, cool accent:

- Three radio boxes stacked: `radio 1 (TX + RX)`, `radio 2 (RX)`,
  `radio 3 (RX)` — with radio 1 visually marked as the only transmitter
- All three feed one box: `waybeam-link (RX)` — internal functions:
  *merge + deduplicate*, *reassemble*, *FEC decode*
- → arrow labelled *shared memory ring* → `Decoder` → `Display`

Below and slightly detached from the ground station, in grey, to show broadcast
is open:

- `Spectator (receive-only)` and `Cache store (receive-only, Ethernet-linked)`,
  each with a thin dotted arrow up to the RF hop, labelled *listen only*

**Caption to render at the bottom:** *"One radio up, many ears down."*

---

## Figure 2 — The video data path  *(README "The video path")*

A strict left-to-right pipeline. This figure is about **transformation of
data**, so make each stage's input and output explicit.

Stages, in order:

1. `Encoder` — output annotated *"one complete encoded frame"*
2. `Shared memory ring` — annotated *"same host, zero copy"*
3. `Fragment` — annotated *"frame → N source symbols, sized to the current
   modulation's payload budget"*
4. `FEC encode` — annotated *"+ M GF(256) Reed–Solomon repair symbols;
   keyframes get a higher rate"* — draw the source symbols as filled blocks and
   the repair symbols as outlined blocks so the difference is visible
5. `Inject` → the RF hop
6. Three inbound arrows from three ground radios converging on
   `Merge + deduplicate` — annotated *"the same symbol heard twice is kept
   once"*
7. `Reassemble + FEC decode` — annotated *"recovers missing symbols from the
   repair set"*
8. `Shared memory ring` → `Decoder`

**Show loss explicitly.** Somewhere between stage 5 and stage 6, draw two or
three symbol blocks in red with a small ✗, and carry a red annotation to stage 7
reading *"recovered"*. This is the single most valuable thing this figure can
communicate.

**Add a callout box** anchored on the final stage: **"The reassembled frame is
byte-identical to what the encoder produced."**

---

## Figure 3 — The adaptive control loop  *(README "The adaptive link layer")*

A **closed loop** drawn as a cycle, not a pipeline. It must read as a circle.

Going clockwise from the top-left:

1. **GROUND — measures** (cool accent). Box listing:
   `RSSI`, `SNR`, `loss before diversity merge`, `loss after diversity merge`,
   `per-adapter health`
2. → dashed arrow labelled **link report (over the return path)** →
3. **VEHICLE — decides** (warm accent). Box titled `selector`, listing three
   behaviours:
   - `demote — reactive, on measured loss`
   - `promote — conservative, on signal margin`
   - `freeze — suppresses oscillation`
4. → three solid arrows fanning out to three actuator boxes:
   - `MCS (modulation rate)`
   - `TX power (per adapter)`
   - `Encoder bitrate`
5. → arrow from the actuators back round to the RF hop and thus back to the
   ground, closing the loop.

**Two annotations that carry the real content:**

- Across the three actuators: **"moved as one coordinated sequence — bitrate
  down before modulation down; modulation up before bitrate up"**
- On the `Encoder bitrate` actuator, boxed and emphasised: **"waybeam-link is
  the SOLE writer of encoder bitrate"**

Optionally, a small inset showing the fail-safe: *"reports stop arriving →
fall back to the most robust operating point."*

---

## Figure 4 — Discovery, pairing and the channel switch  *(README "Discovery, pairing and the follow-me channel switch")*

Best drawn as a **sequence diagram** — two vertical lifelines, `GROUND` on the
left and `VEHICLE` on the right, time flowing downward. Three labelled phases
separated by horizontal rules.

**Phase 1 — DISCOVER**
- Vehicle → (broadcast, into space): `announcement, every 500 ms`
- Ground: self-directed activity box labelled `scan channel list — dwell long
  enough on each to actually hear one`
- Vehicle → Ground: `announcement heard on channel N`

**Phase 2 — CLAIM**
- Ground → Vehicle: `claim (token-keyed)`
- Vehicle: activity box `bind to this originator as command source`
- Annotation: *"from here on, the vehicle accepts control only from this ground
  station"*

**Phase 3 — FOLLOW-ME CHANNEL SWITCH**
- Ground → Vehicle: `switch to channel M (authenticated)`
- Vehicle → Ground: `acknowledge`
- Annotation on the ground lifeline: *"ground does not commit until it has the
  acknowledgement"*
- Both lifelines: a synchronised box labelled `switch, scheduled against the
  radio's hardware clock — both sides arrive together`
- Then a fork, drawn as two outcomes:
  - **Success** (green/neutral): `link proven on channel M → commit and hold`
  - **Failure** (red): `vehicle does not appear → automatic back-out to
    channel N`

The back-out branch must be as visually prominent as the success branch. It is
the point of the whole mechanism.

---

## Figure 5 — Control plane and observability  *(README "Control plane and observability")*

The smallest figure. Shows how an operator or another program touches a running
node.

Centre: a box `waybeam-link node (single event loop)`, annotated *"no threads,
no locks — the control surface runs in the same loop as the transport"*.

Into it from the left, dashed arrows:
- `REST client / curl` → `POST /api/v1/...` — labelled *live, no restart*
- `Ground application (WebUI)` → `GET /api/v1/stats/stream (SSE)`

Out of it to the right, solid arrows:
- → `newline-delimited JSON statistics, N times per second` → into a box
  `link_monitor.py` → into a browser-window shape labelled `fleet dashboard`
- Show two or three node boxes all feeding the same `link_monitor.py`, to make
  the point that it is a *fleet* view.

Small annotation on the node box: *"deployed convention — :8091 on a vehicle,
:8092 on a ground station"*.

Keep this one deliberately plain; it is a supporting figure, not a hero.

---

## If only ONE figure can be produced

Produce **Figure 1**. It is the only one whose absence would leave the README
without an architectural anchor. Figures 2 and 3 are the next most valuable, in
that order.
