// SPDX-License-Identifier: GPL-2.0-or-later
// §15.2 (Pass 195) auto-adapter election: ranking, tiebreak, cap and stanza
// synthesis. Every case is driven from SYNTHETIC candidates — the election is
// the part of auto with policy in it, and pinning it to whatever radios happen
// to be plugged into the author's machine would test the machine, not the
// rule. Nothing here opens USB, so it runs in builds with no radio at all.
#include <algorithm>
#include <string>
#include <vector>

#include "wblink/adapter_elect.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// The four parts the seed order names, as devourer reports them.
AdapterCandidate eu(const char* mac, const char* path = "1-1") {
    return {"RTL8822E", "RTL8812EU/RTL8822EU", mac, path};
}
AdapterCandidate au(const char* mac, const char* path = "1-2") {
    return {"RTL8812A", "RTL8812AU/RTL8812AR", mac, path};
}
AdapterCandidate cu(const char* mac, const char* path = "1-3") {
    return {"RTL8822C", "RTL8812CU/RTL8822CU", mac, path};
}
AdapterCandidate bu(const char* mac, const char* path = "1-4") {
    return {"RTL8733B", "RTL8731BU/RTL8733BU", mac, path};
}

AdapterAutoCfg cfg_at(uint16_t chan = 5805, uint8_t max_adapters = 4) {
    AdapterAutoCfg c;
    c.enabled = true;
    c.channel_mhz = chan;
    c.bw = 20;
    c.max_adapters = max_adapters;
    return c;
}

// Convenience: which candidate index transmits, for a one-line assertion.
std::size_t elected(const std::vector<AdapterCandidate>& cands,
                    const AdapterAutoCfg& c) {
    return plan_adapters(cands, c, /*elect_tx=*/true).tx;
}

}  // namespace

int main() {
    // --- alias matching: the operator writes what is on the dongle ---------
    {
        const AdapterCandidate c = eu("aa:aa:aa:aa:aa:aa");
        CHECK(part_matches(c, "8812EU"));    // marketing token
        CHECK(part_matches(c, "RTL8812EU")); // with the vendor prefix
        CHECK(part_matches(c, "rtl8812eu")); // case-insensitive
        CHECK(part_matches(c, "8822EU"));    // the other alias on the same die
        CHECK(part_matches(c, "RTL8822E"));  // the die name devourer reports
        CHECK(part_matches(c, "8822E"));
        // An 8822E is NOT an 8822C, however similar the strings look — this is
        // the pair a substring match would silently conflate.
        CHECK(!part_matches(c, "8812CU"));
        CHECK(!part_matches(c, "RTL8822C"));
        CHECK(!part_matches(c, ""));
        // A unit devourer could not identify matches nothing, rather than
        // matching everything.
        const AdapterCandidate blank{"", "", "bb:bb:bb:bb:bb:bb", "2-1"};
        CHECK(!part_matches(blank, "8812EU"));
        CHECK(!part_matches(blank, "RTL8822E"));
    }

    // --- rank: seed order, and unlisted parts sort last --------------------
    {
        const std::vector<std::string>& p = default_tx_priority();
        CHECK_EQ_U(p.size(), 4u);
        CHECK_EQ_U(tx_priority_rank(eu("a"), p), 0u);
        CHECK_EQ_U(tx_priority_rank(au("a"), p), 1u);
        CHECK_EQ_U(tx_priority_rank(cu("a"), p), 2u);
        CHECK_EQ_U(tx_priority_rank(bu("a"), p), 3u);
        // 8821C is supported silicon that nobody ranked: it is claimable as
        // diversity, so it gets the sentinel rank rather than being refused.
        const AdapterCandidate other{"RTL8821C", "RTL8821CU", "c", "9-9"};
        CHECK_EQ_U(tx_priority_rank(other, p), p.size());
    }

    // --- election picks by part, not by claim order ------------------------
    {
        // AU enumerated first, EU second: the higher-priority part must win
        // even though the bus handed it over later.
        const std::vector<AdapterCandidate> cands = {au("22:22:22:22:22:22"),
                                                     eu("11:11:11:11:11:11")};
        CHECK_EQ_U(elected(cands, cfg_at()), 1u);
    }
    {
        // Reverse the enumeration order; the SAME physical unit must win.
        const std::vector<AdapterCandidate> cands = {eu("11:11:11:11:11:11"),
                                                     au("22:22:22:22:22:22")};
        CHECK_EQ_U(elected(cands, cfg_at()), 0u);
    }
    {
        // Full seed order over all four, shuffled.
        const std::vector<AdapterCandidate> cands = {
            bu("44:44:44:44:44:44"), cu("33:33:33:33:33:33"),
            au("22:22:22:22:22:22"), eu("11:11:11:11:11:11")};
        AdapterAutoCfg c = cfg_at(5805, 0);  // uncapped, so nothing is dropped
        CHECK_EQ_U(elected(cands, c), 3u);
        CHECK_EQ_U(plan_adapters(cands, c, true).keep.size(), 4u);
    }

    // --- an operator override beats the seed -------------------------------
    {
        const std::vector<AdapterCandidate> cands = {eu("11:11:11:11:11:11"),
                                                     bu("44:44:44:44:44:44")};
        AdapterAutoCfg c = cfg_at();
        c.tx_priority = {"8733BU", "8812EU"};
        CHECK_EQ_U(elected(cands, c), 1u);
    }

    // --- tiebreak: MAC ascending, never the USB path -----------------------
    {
        // Two of the same part. The path order says unit 0 first; the MAC says
        // unit 1. The MAC must decide, because a re-plug reshuffles paths and
        // an election that followed them would move the uplink for no reason.
        const std::vector<AdapterCandidate> cands = {
            eu("bb:bb:bb:bb:bb:bb", "1-1"), eu("aa:aa:aa:aa:aa:aa", "1-2")};
        CHECK_EQ_U(elected(cands, cfg_at()), 1u);
        // Swap the paths: same answer. This is the stability claim itself.
        const std::vector<AdapterCandidate> replugged = {
            eu("bb:bb:bb:bb:bb:bb", "5-4"), eu("aa:aa:aa:aa:aa:aa", "2-1")};
        CHECK_EQ_U(elected(replugged, cfg_at()), 1u);
    }
    {
        // An identity-less unit sorts AFTER one that has a MAC in the same
        // tier. An empty string is not a low MAC: handing the uplink to the
        // one unit whose calibration can never be keyed (Pass 154 D3) is the
        // opposite of what the tiebreak is for.
        const std::vector<AdapterCandidate> cands = {eu("", "1-1"),
                                                     eu("ff:ff:ff:ff:ff:ff")};
        CHECK_EQ_U(elected(cands, cfg_at()), 1u);
    }
    {
        // Both identity-less: still deterministic, by path, because an
        // arbitrary-but-stable answer beats an arbitrary one.
        const std::vector<AdapterCandidate> cands = {eu("", "1-9"),
                                                     eu("", "1-2")};
        CHECK_EQ_U(elected(cands, cfg_at()), 1u);
    }

    // --- the cap keeps the BEST N, not the first N -------------------------
    {
        // Bus order puts the two worst parts first. A cap of 2 applied at
        // enumeration would keep exactly those and throw away both good ones.
        const std::vector<AdapterCandidate> cands = {
            bu("44:44:44:44:44:44"), cu("33:33:33:33:33:33"),
            au("22:22:22:22:22:22"), eu("11:11:11:11:11:11")};
        const AdapterPlan plan = plan_adapters(cands, cfg_at(5805, 2), true);
        CHECK_EQ_U(plan.keep.size(), 2u);
        // EU (index 3) and AU (index 2), reported in CLAIM order so the caller
        // never has to move a brought-up unit.
        CHECK_EQ_U(plan.keep[0], 2u);
        CHECK_EQ_U(plan.keep[1], 3u);
        CHECK_EQ_U(plan.tx, 3u);
        // The elected unit is always inside the kept set — a plan that
        // transmits on a dropped adapter is not a plan.
        CHECK(std::find(plan.keep.begin(), plan.keep.end(), plan.tx) !=
              plan.keep.end());
    }
    {
        // 0 = no cap.
        const std::vector<AdapterCandidate> cands = {
            bu("4"), cu("3"), au("2"), eu("1"), eu("0")};
        CHECK_EQ_U(plan_adapters(cands, cfg_at(5805, 0), true).keep.size(), 5u);
    }
    {
        // A cap above the candidate count keeps everything rather than
        // indexing past the end.
        const std::vector<AdapterCandidate> cands = {eu("1")};
        CHECK_EQ_U(plan_adapters(cands, cfg_at(5805, 4), true).keep.size(), 1u);
    }

    // --- rx-only archetypes elect no TX ------------------------------------
    {
        const std::vector<AdapterCandidate> cands = {eu("1"), au("2")};
        const AdapterPlan plan = plan_adapters(cands, cfg_at(), false);
        CHECK_EQ_U(plan.tx, kNoTx);
        CHECK_EQ_U(plan.keep.size(), 2u);
        const std::vector<AdapterCfg> st = auto_stanzas(cands, plan, cfg_at());
        CHECK_EQ_U(st.size(), 2u);
        CHECK(st[0].role == Role::kRx);
        CHECK(st[1].role == Role::kRx);
    }

    // --- no candidates: an empty plan, not a crash and not a phantom TX ----
    {
        const std::vector<AdapterCandidate> none;
        const AdapterPlan plan = plan_adapters(none, cfg_at(), true);
        CHECK_EQ_U(plan.tx, kNoTx);
        CHECK(plan.keep.empty());
        CHECK(auto_stanzas(none, plan, cfg_at()).empty());
    }

    // --- synthesized stanzas -----------------------------------------------
    {
        const std::vector<AdapterCandidate> cands = {au("22:22:22:22:22:22"),
                                                     eu("11:11:11:11:11:11")};
        AdapterAutoCfg c = cfg_at(5745);
        c.bw = 40;
        // Power keys reach the ELECTED unit and nothing else — the same rule
        // config load enforces on a hand-authored role:"rx" stanza.
        c.tx_template.power_offset_qdb = -96;
        c.tx_template.power_offset_max_qdb = 16;
        c.tx_template.power_offset_presets_qdb = {-96, -48, 0};
        c.tx_template.power_map = "/etc/waybeam-link/power.txt";

        const AdapterPlan plan = plan_adapters(cands, c, true);
        const std::vector<AdapterCfg> st = auto_stanzas(cands, plan, c);
        CHECK_EQ_U(st.size(), 2u);

        // Names carry the CLAIM index and the part an operator recognises, and
        // are unique by construction.
        CHECK(st[0].name == "auto0-8812au");
        CHECK(st[1].name == "auto1-8812eu");
        CHECK(st[0].name != st[1].name);

        CHECK(st[0].role == Role::kRx);
        CHECK(st[1].role == Role::kTx);
        // Identity is stamped on both, so `adapters --emit` produces a
        // directly pasteable pinned array and /api/v1/info can name the unit
        // behind each role.
        CHECK(st[0].mac == "22:22:22:22:22:22");
        CHECK(st[1].mac == "11:11:11:11:11:11");
        // Channel and bw come from the auto block, once, for every stanza.
        CHECK_EQ_U(st[0].channel_mhz, 5745u);
        CHECK_EQ_U(st[1].channel_mhz, 5745u);
        CHECK_EQ_U(st[0].bw, 40u);
        CHECK_EQ_U(st[1].bw, 40u);

        // The diversity ear keeps the SAFE DEFAULTS, not the template: a
        // -96 qdb backoff authored for the transmitting unit means nothing on
        // an ear, and power_map on a role:"rx" stanza is a load-time error the
        // array form would have refused.
        CHECK_EQ_U(st[1].power_offset_qdb + 512, static_cast<int32_t>(-96 + 512));
        CHECK_EQ_U(st[1].power_offset_max_qdb + 512, 16 + 512);
        CHECK_EQ_U(st[1].power_offset_presets_qdb.size(), 3u);
        CHECK(st[1].power_map == "/etc/waybeam-link/power.txt");
        CHECK_EQ_U(st[0].power_offset_qdb + 512, static_cast<int32_t>(-24 + 512));
        CHECK_EQ_U(st[0].power_offset_max_qdb + 512, 0 + 512);
        CHECK(st[0].power_offset_presets_qdb.empty());
        CHECK(st[0].power_map.empty());
    }
    {
        // Two units of one part still get distinct names — the claim index,
        // not a running counter over the kept set, is what guarantees it even
        // when the cap drops something in between.
        const std::vector<AdapterCandidate> cands = {
            eu("11:11:11:11:11:11", "1-1"), bu("44:44:44:44:44:44", "1-4"),
            eu("22:22:22:22:22:22", "1-2")};
        const AdapterPlan plan = plan_adapters(cands, cfg_at(5805, 2), true);
        const std::vector<AdapterCfg> st = auto_stanzas(cands, plan, cfg_at());
        CHECK_EQ_U(st.size(), 2u);
        CHECK(st[0].name == "auto0-8812eu");
        CHECK(st[1].name == "auto2-8812eu");
        CHECK(st[0].name != st[1].name);
    }
    {
        // An unidentified unit still gets a usable, unique name.
        const std::vector<AdapterCandidate> cands = {{"", "", "", "3-1"}};
        const AdapterPlan plan = plan_adapters(cands, cfg_at(), true);
        const std::vector<AdapterCfg> st = auto_stanzas(cands, plan, cfg_at());
        CHECK_EQ_U(st.size(), 1u);
        CHECK(st[0].name == "auto0-unknown");
        CHECK(st[0].role == Role::kTx);
    }

    // --- the declaration names every candidate, including the rejects ------
    {
        const std::vector<AdapterCandidate> cands = {
            bu("44:44:44:44:44:44", "1-4"), eu("11:11:11:11:11:11", "1-1")};
        const AdapterAutoCfg c = cfg_at(5805, 1);
        const AdapterPlan plan = plan_adapters(cands, c, true);
        const std::string d = describe_plan(cands, plan, c);
        CHECK(d.find("RTL8822E") != std::string::npos);
        CHECK(d.find("11:11:11:11:11:11") != std::string::npos);
        CHECK(d.find("ELECTED UPLINK") != std::string::npos);
        // The dropped unit must appear WITH its reason: a table that lists
        // only the winners cannot answer "why is that one not my TX".
        CHECK(d.find("RTL8733B") != std::string::npos);
        CHECK(d.find("DROPPED") != std::string::npos);
        CHECK(d.find("1-4") != std::string::npos);
    }
    {
        // An unlisted part reads "unlisted", not a rank equal to the list
        // length — printing the sentinel as a number invites reading it as a
        // real position.
        const std::vector<AdapterCandidate> cands = {
            {"RTL8821C", "RTL8821CU", "c1:c1:c1:c1:c1:c1", "2-2"}};
        const AdapterAutoCfg c = cfg_at();
        const std::string d =
            describe_plan(cands, plan_adapters(cands, c, true), c);
        CHECK(d.find("unlisted") != std::string::npos);
        CHECK(d.find("rank 4") == std::string::npos);
    }
    {
        // The rx-only declaration says WHY there is no uplink.
        const std::vector<AdapterCandidate> cands = {eu("11:11:11:11:11:11")};
        const AdapterAutoCfg c = cfg_at();
        const std::string d =
            describe_plan(cands, plan_adapters(cands, c, false), c);
        CHECK(d.find("no tx elected") != std::string::npos);
        CHECK(d.find("ELECTED UPLINK") == std::string::npos);
    }

    return wbtest_finish("adapter_elect");
}
