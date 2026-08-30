// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §15.2 (Pass 195) auto-adapter election.
//
// PURE. Nothing here opens USB, reads a file or touches devourer — it takes a
// list of already-identified units and returns which to keep, which transmits,
// and the stanza array that describes the outcome. That is deliberate: the
// election is the part with policy in it, and a rule the bench cannot reach is
// a rule nobody checks. `tests/adapter_elect_test.cpp` drives every case from
// synthetic candidates, so the ranking is covered in builds with no radio at
// all (and unit tests must never enumerate hardware).
//
// WHY THE CALLER MUST BRING UP FIRST. The two highest-priority parts,
// RTL8812EU and RTL8812AU, share USB PID 0x8812 (see
// third_party/devourer/src/WiFiDriver.cpp): the family is only knowable from
// SYS_CFG2, read inside CreateRtlDevice, and the EFUSE MAC only during
// InitWrite. So an AdapterCandidate can only be filled in AFTER the unit is
// up — which is why auto rides the Pass 154 claim/bring-up/identify/re-bind
// sequence rather than adding one of its own.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "wblink/config.h"

namespace wblink {

// One physical unit, as the backend found it after bring-up.
struct AdapterCandidate {
    // devourer AdapterCaps::chip_name — the silicon die, no bus suffix:
    // "RTL8822E", "RTL8812A", "RTL8822C", "RTL8733B". Empty = unidentified.
    std::string part;
    // AdapterCaps::marketing_names — a '/'-separated alias list, e.g.
    // "RTL8812EU/RTL8822EU". This is what an operator has actually written on
    // the dongle, so it is matched alongside `part`.
    std::string aliases;
    // §10.6 EFUSE identity, lowercase "aa:bb:cc:dd:ee:ff". EMPTY when the unit
    // reports none (Pass 154 D3) — such a unit still flies, but it sorts last
    // within its priority tier because it has no stable key to sort on.
    std::string mac;
    // USB port path, or "fd:<n>" for a caller-supplied descriptor. Diagnostic
    // and last-resort tiebreak only: it shuffles on re-plug, which is exactly
    // why it must never decide the election.
    std::string path;
};

// The §15.2 seed order, best TX first. A TIER-2 SEED — no head-to-head TX
// comparison of these four parts exists; see docs/findings.md 2026-08-30 for
// what is and is not known. `adapters.auto.tx_priority` overrides it.
const std::vector<std::string>& default_tx_priority();

// True when `want` names this candidate's die. Case-insensitive, with an
// optional "RTL" prefix on either side, matched against `part` and every
// '/'-separated token of `aliases` — so "8812EU", "RTL8812EU" and "RTL8822E"
// all name the same unit. An empty `want` matches nothing.
bool part_matches(const AdapterCandidate& c, const std::string& want);

// Position of the first `priority` entry naming this candidate, or
// priority.size() when unlisted. Unlisted parts therefore rank below every
// listed one rather than being refused: a supported radio nobody ranked is
// still worth more as diversity than not claimed at all.
std::size_t tx_priority_rank(const AdapterCandidate& c,
                             const std::vector<std::string>& priority);

inline constexpr std::size_t kNoTx = static_cast<std::size_t>(-1);

struct AdapterPlan {
    // Candidate indices to keep, ASCENDING — i.e. claim order is preserved, so
    // a kept unit never has to be moved and its already-running bring-up state
    // stays where the backend put it. Ranking decides membership, not order.
    std::vector<std::size_t> keep;
    // Candidate index elected to transmit, or kNoTx. Always a member of
    // `keep`; kNoTx exactly when elect_tx is false or there are no candidates.
    std::size_t tx = kNoTx;
};

// Rank, cap and elect. `elect_tx` is false for the §3.11 uplink-free
// archetypes (node.spectator, or a cache store with no streams), which are
// precisely the nodes allowed zero role:"tx" adapters — auto reads that from
// the archetype the spec already defines rather than from a key of its own.
//
// Ordering within the plan: priority rank ascending, then MAC ascending, then
// path ascending. The MAC tiebreak is what makes an election stable across a
// re-plug; the path leg is only reachable when two units both report no
// identity at all, and exists so the result is still deterministic there.
//
// cfg.max_adapters caps AFTER ranking, so it keeps the best N.
AdapterPlan plan_adapters(const std::vector<AdapterCandidate>& cands,
                          const AdapterAutoCfg& cfg, bool elect_tx);

// The stanza array the plan describes, parallel to `plan.keep`. Names are
// "auto<claim-index>-<alias>" (e.g. "auto0-8812eu"), which is unique by
// construction — two dongles of one part must not collide on a name.
// cfg.tx_template's power fields land on the elected stanza only, exactly as
// they would on a hand-authored role:"tx" stanza.
std::vector<AdapterCfg> auto_stanzas(const std::vector<AdapterCandidate>& cands,
                                     const AdapterPlan& plan,
                                     const AdapterAutoCfg& cfg);

// The bring-up declaration: every candidate, its part, identity, path, rank
// and assigned role — including the ones the cap dropped. §15.2 requires auto
// to declare its outcome rather than infer it silently, and a table that omits
// the rejects cannot answer "why is that one not my TX". Multi-line, newline
// terminated; pure so the format is testable.
std::string describe_plan(const std::vector<AdapterCandidate>& cands,
                          const AdapterPlan& plan, const AdapterAutoCfg& cfg);

}  // namespace wblink
