# Review log

Numbered record of **Tier-1 spec rulings** — the contract law of `PROTOCOL.md`.
Passes 1–152 (2026-07-10 → 2026-08-06) live in
`review-log-archive-p001-152.md`; this file continues the numbering from
Pass 153. The two-tier split itself is defined in `CLAUDE.md` ("The law").

## Format contract

- **Tier-1 rulings only.** An entry exists because a *contract* changed: wire
  format, trust/auth machinery, a state-machine behaviour peers depend on, or
  config semantics. Measurement-phase results — walls, gates, dwell counts,
  seeds, sweep parameters still being characterised — go to `findings.md`,
  not here, and not into `PROTOCOL.md`.
- **One numbered Pass per entry, ≤40 lines**: the verdict, the changed spec
  sections (`§N.M`), and an evidence pointer (branch, data dir, findings
  entry). No narration, no method essays.
- **No addenda.** A new fact after an entry is committed is a new Pass or a
  new finding — an entry is never reopened.
- **Method lessons** (process traps, measurement discipline) go to the
  coordination repo's memory, not this log.
- Spec-amendment-commits-first still applies to every entry here; it does
  **not** apply to findings.

## Passes

<!-- Pass 153 goes here. -->
