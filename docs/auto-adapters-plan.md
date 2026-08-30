# Auto-adapters — SHIPPED (Pass 195, 2026-08-30)

This file was a design draft (2026-07-30). The feature has landed, so the draft
is retired rather than left beside the thing it described — a third copy of a
settled design is exactly the drift `CLAUDE.md` warns about. Where it now lives:

| What | Where |
|---|---|
| The contract — config shape, election law, enumeration filter, refusals | `PROTOCOL.md` §15.2 (`adapters` object form) |
| Artifact keying across adapter swaps | `PROTOCOL.md` §10.6 (AMENDED Pass 195) |
| `home_chan` liveness | `PROTOCOL.md` §11.5 (CLARIFIED Pass 195) |
| Why it is built this way | `docs/review-log.md` Pass 195 |
| Why the TX priority order is a seed and not a result | `docs/findings.md` 2026-08-30 |
| The election itself, as testable code | `io/include/wblink/adapter_elect.h` |

## What the draft got wrong, kept as a record

Four of the draft's premises expired between it and the implementation. They
are listed because each one would have produced a worse feature:

1. **"auto mode always elects a TX; RX-only radio is a parity gap."** `§3.11`
   `allow_rx_only` (Pass 162) landed in between. Auto now elects a TX *unless*
   the node is an uplink-free archetype, which it reads from `node.spectator` /
   cache-store rather than from a key of its own.
2. **"auto with kernel-monitor is a config error."** kernel-monitor was deleted
   (Pass 164). devourer is the only backend, so the refusal is against the udp
   dev backend instead.
3. **"tiebreak by EFUSE MAC — never by USB bus path."** Correct, but the draft
   had no way to *get* a MAC: Pass 154 added `GetPermanentMacAddress` at
   bring-up and the re-bind that consumes it. Auto is one branch on that
   machinery, not a new mechanism.
4. **No Android path.** `adapter_fds` (Pass 173) arrived after the draft. The
   shipped rule — the fd list *is* the device set when non-empty, otherwise
   enumerate — is what lets an unrooted phone reuse the election with no new
   C ABI call.

The draft also proposed a **USB PID allowlist** to keep the probe from opening
non-radio Realtek devices. Shipped instead: a filter on the interface
descriptor (vendor-specific class with bulk IN+OUT). Same protection, and it
does not go stale as new PIDs appear. The draft's own PID list was already
missing parts devourer had gained by the time it was implemented.

Its open decisions D1–D4 were resolved by operator ruling on 2026-08-30 and are
recorded in Pass 195.
