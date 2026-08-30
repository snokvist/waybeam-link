// SPDX-License-Identifier: GPL-2.0-or-later
// §15.2 (Pass 195) auto-adapter election. Pure — see adapter_elect.h.
#include "wblink/adapter_elect.h"

#include <algorithm>
#include <cstdio>

namespace wblink {
namespace {

// Uppercase and drop a leading "RTL", so "8812eu", "RTL8812EU" and the
// marketing token "RTL8812EU" all normalize to "8812EU". Matching on the
// normalized form is what lets an operator write the name printed on the
// dongle instead of the die name devourer reports.
std::string norm_part(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (s.rfind("RTL", 0) == 0) s.erase(0, 3);
    return s;
}

// Split "RTL8812EU/RTL8822EU" on '/'. Empty tokens are dropped so a trailing
// or doubled separator cannot produce a token that matches an empty `want`.
std::vector<std::string> alias_tokens(const std::string& aliases) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : aliases) {
        if (c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Lowercased part name for the synthesized stanza, in the spelling the
// OPERATOR uses. Prefers whichever alias the priority list actually named, so
// a stanza cannot disagree with the election table that produced it: an
// RTL8733B reports "RTL8731BU/RTL8733BU", and taking the first token blindly
// named an 8733BU dongle `autoN-8731bu` while the table said it matched
// "8733BU". Falls back to the first alias, then to the die name.
std::string short_name(const AdapterCandidate& c,
                       const std::vector<std::string>& priority) {
    const std::vector<std::string> toks = alias_tokens(c.aliases);
    std::string s;
    for (const std::string& want : priority) {
        if (part_matches(c, want)) {
            s = norm_part(want);
            break;
        }
    }
    if (s.empty()) {
        s = toks.empty() ? norm_part(c.part) : norm_part(toks.front());
    }
    if (s.empty()) return "unknown";
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

}  // namespace

const std::vector<std::string>& default_tx_priority() {
    // Function-local static, not a namespace-scope object: this is read during
    // config load, and a namespace-scope vector in another TU would be an
    // initialization-order hazard for no gain.
    static const std::vector<std::string> kSeed = {"8812EU", "8812AU",
                                                   "8812CU", "8733BU"};
    return kSeed;
}

bool part_matches(const AdapterCandidate& c, const std::string& want) {
    if (want.empty()) return false;
    const std::string w = norm_part(want);
    if (w.empty()) return false;
    if (!c.part.empty() && norm_part(c.part) == w) return true;
    for (const std::string& t : alias_tokens(c.aliases)) {
        if (norm_part(t) == w) return true;
    }
    return false;
}

std::size_t tx_priority_rank(const AdapterCandidate& c,
                             const std::vector<std::string>& priority) {
    for (std::size_t i = 0; i < priority.size(); ++i) {
        if (part_matches(c, priority[i])) return i;
    }
    return priority.size();
}

AdapterPlan plan_adapters(const std::vector<AdapterCandidate>& cands,
                          const AdapterAutoCfg& cfg, bool elect_tx) {
    AdapterPlan plan;
    if (cands.empty()) return plan;

    const std::vector<std::string>& prio =
        cfg.tx_priority.empty() ? default_tx_priority() : cfg.tx_priority;

    // Best-first order. A unit with no EFUSE identity sorts after one that has
    // it within the same tier: an empty MAC is not a low MAC, and letting it
    // win a tiebreak would hand the uplink to the one unit whose calibration
    // can never be keyed (Pass 154 D3).
    std::vector<std::size_t> ranked(cands.size());
    for (std::size_t i = 0; i < ranked.size(); ++i) ranked[i] = i;
    std::stable_sort(
        ranked.begin(), ranked.end(),
        [&](std::size_t a, std::size_t b) {
            const std::size_t ra = tx_priority_rank(cands[a], prio);
            const std::size_t rb = tx_priority_rank(cands[b], prio);
            if (ra != rb) return ra < rb;
            const bool ha = !cands[a].mac.empty();
            const bool hb = !cands[b].mac.empty();
            if (ha != hb) return ha;
            if (cands[a].mac != cands[b].mac) return cands[a].mac < cands[b].mac;
            return cands[a].path < cands[b].path;
        });

    // Cap AFTER ranking (§15.2): keep the best N, not the first N the bus
    // happened to enumerate. 0 means uncapped.
    std::size_t keep_n = ranked.size();
    if (cfg.max_adapters != 0 && cfg.max_adapters < keep_n) {
        keep_n = cfg.max_adapters;
    }
    if (elect_tx) plan.tx = ranked.front();

    plan.keep.assign(
        ranked.begin(),
        ranked.begin() +
            static_cast<std::vector<std::size_t>::difference_type>(keep_n));
    // Claim order, so a kept unit never moves relative to the backend's
    // already-brought-up array. The cap decided membership; it does not get to
    // reorder what survived.
    std::sort(plan.keep.begin(), plan.keep.end());
    return plan;
}

std::vector<AdapterCfg> auto_stanzas(const std::vector<AdapterCandidate>& cands,
                                     const AdapterPlan& plan,
                                     const AdapterAutoCfg& cfg) {
    const std::vector<std::string>& prio =
        cfg.tx_priority.empty() ? default_tx_priority() : cfg.tx_priority;
    std::vector<AdapterCfg> out;
    out.reserve(plan.keep.size());
    for (const std::size_t i : plan.keep) {
        if (i >= cands.size()) continue;  // defensive; plan_adapters never does
        const AdapterCandidate& c = cands[i];
        AdapterCfg a;
        // Indexed like `cands`, which is the SURVIVING claimed set — the same
        // space describe_plan's [N] uses, so the table and the stanza names
        // always agree. Not the enumeration index: a skipped candidate shifts
        // those, which is why the skip log names a path instead.
        a.name = "auto" + std::to_string(i) + "-" + short_name(c, prio);
        // Descriptive, not a pin — the binding already happened. It is stamped
        // so `adapters --emit` produces a directly pasteable pinned array and
        // so GET /api/v1/info reports the identity behind each role.
        a.mac = c.mac;
        a.role = (i == plan.tx) ? Role::kTx : Role::kRx;
        a.channel_mhz = cfg.channel_mhz;
        a.bw = cfg.bw;
        if (a.role == Role::kTx) {
            // Power keys reach the elected unit only, exactly as they would on
            // a hand-authored role:"tx" stanza — config load already refuses
            // them on a role:"rx" one, and auto must not smuggle them past it.
            const AdapterCfg& t = cfg.tx_template;
            a.power_map = t.power_map;
            a.max_power_qdb = t.max_power_qdb;
            a.power_offset_qdb = t.power_offset_qdb;
            a.power_offset_max_qdb = t.power_offset_max_qdb;
            a.power_presets_qdb = t.power_presets_qdb;
            a.power_offset_presets_qdb = t.power_offset_presets_qdb;
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::string describe_plan(const std::vector<AdapterCandidate>& cands,
                          const AdapterPlan& plan, const AdapterAutoCfg& cfg) {
    const std::vector<std::string>& prio =
        cfg.tx_priority.empty() ? default_tx_priority() : cfg.tx_priority;
    std::string s = "adapters: auto election over ";
    s += std::to_string(cands.size());
    s += " candidate(s), channel ";
    s += std::to_string(cfg.channel_mhz);
    s += " MHz bw ";
    s += std::to_string(static_cast<unsigned>(cfg.bw));
    s += ", priority [";
    for (std::size_t i = 0; i < prio.size(); ++i) {
        if (i != 0) s += ' ';
        s += prio[i];
    }
    s += "]\n";
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const AdapterCandidate& c = cands[i];
        const bool kept =
            std::find(plan.keep.begin(), plan.keep.end(), i) != plan.keep.end();
        char buf[64];
        std::snprintf(buf, sizeof buf, "  [%zu] ", i);
        s += buf;
        s += c.part.empty() ? "unknown-part" : c.part;
        s += " (";
        s += c.aliases.empty() ? "no aliases" : c.aliases;
        s += ") mac ";
        s += c.mac.empty() ? "none" : c.mac;
        s += " at ";
        s += c.path.empty() ? "?" : c.path;
        const std::size_t r = tx_priority_rank(c, prio);
        // "unlisted" rather than a number equal to the list length: printing
        // the sentinel as a rank invites reading it as a real position.
        s += r < prio.size() ? " rank " + std::to_string(r) : std::string(" unlisted");
        if (!kept) {
            s += " -> DROPPED (max_adapters " +
                 std::to_string(static_cast<unsigned>(cfg.max_adapters)) + ")";
        } else if (i == plan.tx) {
            s += " -> role tx (ELECTED UPLINK)";
        } else {
            s += " -> role rx";
        }
        s += "\n";
    }
    if (plan.tx == kNoTx && !cands.empty()) {
        s += "  no tx elected: this node is a §3.11 uplink-free archetype "
             "(node.spectator, or a cache store with no streams)\n";
    }
    return s;
}

}  // namespace wblink
