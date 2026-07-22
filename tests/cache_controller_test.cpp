// SPDX-License-Identifier: GPL-2.0-or-later
// CacheController (§14.3) tests: local-collection close rules, futility cap,
// eligibility/ranking, sequential attempts, reply validation, and the
// FrameReassembler repair_candidates snapshot they consume.
#include "wblink/cache_controller.h"

#include <vector>

#include "wblink/endian.h"
#include "wblink/frame_reassembler.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

const StreamKey kTarget{17, 42, 3};

CacheControllerConfig base_cfg() {
    CacheControllerConfig cfg;
    cfg.self_originator = 9;
    cfg.self_session = 77;
    cfg.caches = {33, 34};
    return cfg;  // spec seeds otherwise
}

CacheStatus fresh_status(uint16_t orig, uint32_t oldest, uint32_t newest,
                         uint16_t health = 1000) {
    CacheStatus s;
    s.prefix = CommonPrefix{orig, 9, 0x1000u + orig};
    s.target_originator = kTarget.originator;
    s.target_session = kTarget.session_id;
    s.target_stream_id = kTarget.stream_id;
    s.oldest_block = oldest;
    s.newest_block = newest;
    s.rx_health_permille = health;
    s.capability_flags = cache_capability::kIpTransport;
    return s;
}

// Candidate: k symbols, `missing` source indices absent, no repairs held.
RepairCandidate candidate(uint32_t block, uint16_t k,
                          const std::vector<uint16_t>& missing,
                          uint64_t first_ms, uint64_t last_new_ms,
                          bool eob = false, uint64_t eob_ms = 0) {
    RepairCandidate c;
    c.block_id = block;
    c.k = k;
    c.unique = static_cast<uint16_t>(k - missing.size());
    c.have_eob = eob;
    c.first_ms = first_ms;
    c.last_new_ms = last_new_ms;
    c.eob_ms = eob_ms;
    for (const uint16_t i : missing) {
        c.missing_sources[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
    return c;
}

// Wrapped source symbol for on_reply.
std::vector<uint8_t> wrapped_source(uint32_t block, uint16_t k, uint16_t idx) {
    DataHeader h;
    h.prefix = CommonPrefix{kTarget.originator, 0, kTarget.session_id};
    h.stream_id = kTarget.stream_id;
    h.stream_type = stream_type::kRtp;
    h.seq = block * 100 + idx;
    h.block_id = block;
    std::vector<uint8_t> payload(kFecSourceSubheaderSize + 8, 0xAB);
    be16_write(payload.data() + kFecSrcOffWindowLen, k);
    be16_write(payload.data() + kFecSrcOffSymIndex, idx);
    std::vector<uint8_t> out(kDataHeaderSize + payload.size());
    out.resize(encode_data(h, payload.data(),
                           static_cast<uint16_t>(payload.size()), out.data(),
                           out.size()));
    return out;
}

DataView view_of(const std::vector<uint8_t>& frame) {
    const Decoded dec = decode(frame.data(), frame.size());
    return std::get<DataView>(dec);
}

}  // namespace

int main() {
    // --- close rules --------------------------------------------------------
    {
        CacheController cc(base_cfg());
        cc.on_status(fresh_status(33, 0, 1000), 100);
        // The same cache may report several tracked streams. A status for a
        // different stream must not overwrite this target's registry entry.
        CacheStatus other = fresh_status(33, 0, 1000);
        other.target_stream_id = 4;
        cc.on_status(other, 101);
        // Status from an unconfigured originator is ignored.
        cc.on_status(fresh_status(55, 0, 1000), 100);

        // Not yet closed: quiet floor (min_collect 4ms) not reached.
        RepairCandidate c = candidate(10, 4, {2}, 100, 101);
        CHECK_EQ_U(cc.tick(102, kTarget, &c, 1).size(), 0);
        // Quiet timeout (2ms) passed but min_collect floor (first+4) binds.
        CHECK_EQ_U(cc.tick(103, kTarget, &c, 1).size(), 0);
        // Floor reached => closed => one request to cache 33.
        auto reqs = cc.tick(104, kTarget, &c, 1);
        CHECK_EQ_U(reqs.size(), 1);
        if (!reqs.empty()) {
            CHECK_EQ_U(reqs[0].cache_originator, 33);
            const Decoded dec =
                decode(reqs[0].frame.data(), reqs[0].frame.size());
            const CacheRequestView* rv = std::get_if<CacheRequestView>(&dec);
            CHECK(rv != nullptr);
            if (rv != nullptr) {
                CHECK_EQ_U(rv->hdr.target_cache, 33);
                CHECK_EQ_U(rv->hdr.block_id, 10);
                CHECK_EQ_U(rv->hdr.window_len, 4);
                CHECK_EQ_U(rv->hdr.max_symbols, 1);  // deficit 1
                CHECK_EQ_U(rv->missing_sources[0], 0x04);
                CHECK_EQ_U(rv->hdr.prefix.originator, 9);
                CHECK_EQ_U(rv->hdr.target_originator, 17);
            }
        }
        CHECK_EQ_U(cc.stats().blocks_closed_deficit, 1);
        CHECK_EQ_U(cc.stats().caches_fresh, 1);
        CHECK(cc.has_fresh_target(33, 17, 104));
        CHECK(!cc.has_fresh_target(34, 17, 104));
        cc.reset_link();
        CHECK(!cc.has_fresh_target(33, 17, 104));
        CHECK_EQ_U(cc.stats().caches_fresh, 0);
    }
    {
        // Tail grace closes before the quiet floor.
        CacheController cc(base_cfg());
        cc.on_status(fresh_status(33, 0, 1000), 100);
        RepairCandidate c = candidate(10, 4, {2}, 100, 100, true, 100);
        CHECK_EQ_U(cc.tick(100, kTarget, &c, 1).size(), 0);  // grace 1ms
        CHECK_EQ_U(cc.tick(101, kTarget, &c, 1).size(), 1);
    }
    {
        // Hard close fires even while new symbols keep arriving.
        CacheController cc(base_cfg());
        cc.on_status(fresh_status(33, 0, 1000), 100);
        RepairCandidate c = candidate(10, 8, {6, 7}, 100, 107);
        CHECK_EQ_U(cc.tick(107, kTarget, &c, 1).size(), 0);
        c.last_new_ms = 108;
        CHECK_EQ_U(cc.tick(108, kTarget, &c, 1).size(), 1);  // first+8
    }

    // --- futility (rule 4) + eligibility (rule 6) --------------------------
    {
        CacheController cc(base_cfg());
        cc.on_status(fresh_status(33, 0, 1000), 100);
        // k=8: cap = min(ceil(8*0.2)=2, 8) = 2; deficit 3 > cap => futile.
        RepairCandidate c = candidate(10, 8, {1, 2, 3}, 100, 100);
        CHECK_EQ_U(cc.tick(120, kTarget, &c, 1).size(), 0);
        CHECK_EQ_U(cc.stats().blocks_futile, 1);
        // Vehicle-ARQ path is not this module's concern: nothing else fires.
        CHECK_EQ_U(cc.tick(121, kTarget, &c, 1).size(), 0);
    }
    {
        CacheController cc(base_cfg());
        // Stale status (age > 1500ms), unhealthy cache, block out of window.
        cc.on_status(fresh_status(33, 0, 1000), 100);
        RepairCandidate c = candidate(10, 4, {2}, 3000, 3000);
        CHECK_EQ_U(cc.tick(3010, kTarget, &c, 1).size(), 0);  // stale
        CHECK_EQ_U(cc.stats().requests_suppressed, 1);
        cc.on_status(fresh_status(33, 0, 1000, 500), 3010);   // unhealthy
        CHECK_EQ_U(cc.tick(3011, kTarget, &c, 1).size(), 0);
        cc.on_status(fresh_status(33, 20, 1000), 3011);       // 10 < oldest
        CHECK_EQ_U(cc.tick(3012, kTarget, &c, 1).size(), 0);
        // §14.3 rule 6: a block NEWER than the stale status newest_block
        // stays eligible (only the oldest bound is enforced).
        cc.on_status(fresh_status(33, 0, 5), 3012);
        CHECK_EQ_U(cc.tick(3013, kTarget, &c, 1).size(), 1);
        // suppressed is counted once per block, not per tick.
        CHECK_EQ_U(cc.stats().requests_suppressed, 1);
    }

    // --- sequential attempts + ranking + budget -----------------------------
    {
        CacheController cc(base_cfg());
        // Cache 34 healthier than 33 => ranked first despite config order.
        cc.on_status(fresh_status(33, 0, 1000, 900), 100);
        cc.on_status(fresh_status(34, 0, 1000, 1000), 100);
        // k=20: cap = min(ceil(20*0.2)=4, 8) = 4; deficit 3.
        RepairCandidate c = candidate(10, 20, {5, 6, 7}, 100, 100);
        auto r1 = cc.tick(110, kTarget, &c, 1);
        CHECK_EQ_U(r1.size(), 1);
        CHECK_EQ_U(r1[0].cache_originator, 34);
        // Second attempt waits out request_timeout_ms (4ms)...
        CHECK_EQ_U(cc.tick(112, kTarget, &c, 1).size(), 0);
        // ...then goes to the remaining cache with the residual budget
        // (cap 4 - first allowance 3 = 1).
        auto r2 = cc.tick(115, kTarget, &c, 1);
        CHECK_EQ_U(r2.size(), 1);
        if (!r2.empty()) {
            CHECK_EQ_U(r2[0].cache_originator, 33);
            const Decoded dec = decode(r2[0].frame.data(), r2[0].frame.size());
            const CacheRequestView* rv = std::get_if<CacheRequestView>(&dec);
            CHECK(rv != nullptr && rv->hdr.max_symbols == 1);
        }
        // max_cache_attempts = 2: no third request.
        CHECK_EQ_U(cc.tick(125, kTarget, &c, 1).size(), 0);
        CHECK_EQ_U(cc.stats().requests, 2);
    }

    // --- reply validation ----------------------------------------------------
    {
        CacheController cc(base_cfg());
        cc.on_status(fresh_status(33, 0, 1000), 100);
        // k=10: cap = min(ceil(10*0.2)=2, 8) = 2 covers the deficit of 2.
        RepairCandidate c = candidate(10, 10, {2, 3}, 100, 100);
        auto reqs = cc.tick(110, kTarget, &c, 1);
        CHECK_EQ_U(reqs.size(), 1);
        const Decoded dec = decode(reqs[0].frame.data(), reqs[0].frame.size());
        const uint32_t rid = std::get<CacheRequestView>(dec).hdr.request_id;
        CHECK_EQ_U(reqs[0].request_id, rid);
        cc.note_request_sent(rid, 110000);

        const auto ok = wrapped_source(10, 10, 2);
        const auto not_missing = wrapped_source(10, 10, 1);
        const auto wrong_block = wrapped_source(11, 10, 2);

        CHECK(cc.on_reply(34, rid, view_of(ok), 110100) ==
              CacheController::ReplyVerdict::kWrongCache);
        CHECK(cc.on_reply(33, rid + 99, view_of(ok), 110200) ==
              CacheController::ReplyVerdict::kUnknownRequest);
        CHECK(cc.on_reply(33, rid, view_of(wrong_block), 110300) ==
              CacheController::ReplyVerdict::kWrongBlock);
        CHECK(cc.on_reply(33, rid, view_of(not_missing), 110400) ==
              CacheController::ReplyVerdict::kNotRequested);
        CHECK(cc.on_reply(33, rid, view_of(ok), 111000) ==
              CacheController::ReplyVerdict::kAccept);
        const auto ok2 = wrapped_source(10, 10, 3);
        CHECK(cc.on_reply(33, rid, view_of(ok2), 111200) ==
              CacheController::ReplyVerdict::kAccept);
        // Allowance (deficit 2) exhausted.
        CHECK(cc.on_reply(33, rid, view_of(ok), 111300) ==
              CacheController::ReplyVerdict::kOverAllowance);
        cc.note_nack_grace_armed();
        cc.note_completed(10, 112500, true);
        CHECK_EQ_U(cc.stats().blocks_repaired, 1);
        CHECK_EQ_U(cc.stats().symbols_accepted, 2);
        CHECK_EQ_U(cc.stats().symbols_rejected, 5);
        CHECK_EQ_U(cc.stats().nack_graces_armed, 1);
        CHECK_EQ_U(cc.stats().blocks_repaired_before_nack, 1);
        CHECK_EQ_U(cc.stats().request_to_first_reply.samples, 1);
        CHECK_EQ_U(cc.stats().request_to_first_reply.p95_us, 1000);
        CHECK_EQ_U(cc.stats().request_to_first_reply.max_us, 1000);
        CHECK_EQ_U(cc.stats().request_to_completion.samples, 1);
        CHECK_EQ_U(cc.stats().request_to_completion.p95_us, 2500);
        CHECK_EQ_U(cc.stats().request_to_completion.max_us, 2500);
        // Completion retires every request for the block; later replies are
        // unknown instead of inflating accepted-symbol telemetry.
        CHECK(cc.on_reply(33, rid, view_of(ok), 113000) ==
              CacheController::ReplyVerdict::kUnknownRequest);

        cc.reset_stats();
        CHECK_EQ_U(cc.stats().request_to_first_reply.samples, 0);
        CHECK_EQ_U(cc.stats().request_to_completion.samples, 0);
    }

    // --- FrameReassembler::repair_candidates -------------------------------
    {
        FrameReassemblerConfig frc;
        FrameReassembler reasm(frc);
        int emitted = 0;
        const FrameReassembler::Emit emit = [&](const uint8_t*, size_t) {
            ++emitted;
        };
        // k=4 block 7: feed sources 0 and 3 (3 = EOB tail).
        const auto s0 = wrapped_source(7, 4, 0);
        const auto s3 = wrapped_source(7, 4, 3);
        const DataView v0 = view_of(s0);
        const DataView v3 = view_of(s3);
        reasm.push(v0.hdr.block_id, v0.hdr.data_flags, v0.payload,
                   v0.payload_len, 1000, emit);
        reasm.push(v3.hdr.block_id,
                   static_cast<uint8_t>(v3.hdr.data_flags |
                                        data_flags::kEndOfBlock),
                   v3.payload, v3.payload_len, 1002, emit);
        RepairCandidate cands[4];
        const size_t n = reasm.repair_candidates(cands, 4);
        CHECK_EQ_U(n, 1);
        if (n == 1) {
            CHECK_EQ_U(cands[0].block_id, 7);
            CHECK_EQ_U(cands[0].k, 4);
            CHECK_EQ_U(cands[0].unique, 2);
            CHECK(cands[0].have_eob);
            CHECK_EQ_U(cands[0].first_ms, 1000);
            CHECK_EQ_U(cands[0].last_new_ms, 1002);
            CHECK_EQ_U(cands[0].eob_ms, 1002);
            CHECK_EQ_U(cands[0].missing_sources[0], 0x06);  // 1, 2 missing
            CHECK_EQ_U(cands[0].have_repairs[0], 0x00);
        }
        // Completing the block removes it from the candidate set.
        const auto s1 = wrapped_source(7, 4, 1);
        const auto s2 = wrapped_source(7, 4, 2);
        const DataView v1 = view_of(s1);
        const DataView v2 = view_of(s2);
        reasm.push(v1.hdr.block_id, v1.hdr.data_flags, v1.payload,
                   v1.payload_len, 1003, emit);
        reasm.push(v2.hdr.block_id, v2.hdr.data_flags, v2.payload,
                   v2.payload_len, 1004, emit);
        CHECK_EQ_U(emitted, 1);
        CHECK_EQ_U(reasm.repair_candidates(cands, 4), 0);
    }

    return wbtest_finish("cache_controller_test");
}
