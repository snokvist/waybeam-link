// SPDX-License-Identifier: GPL-2.0-or-later
// FrameReassembler (§6.3a) end-to-end: run FrameFramer (§5.1a) to produce a
// block's symbols, feed them (with loss / reorder / duplication) to the
// reassembler, and assert it emits the byte-exact original blob — or nothing,
// when a frame is unrecoverable (no corrupt partial frames).
#include "wblink/frame_reassembler.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "wblink/frame_framer.h"
#include "wblink/frame_shm_format.h"
#include "wblink/endian.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Sym {
    uint32_t block_id;
    uint8_t flags;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> make_frame(size_t body, bool idr, uint8_t seed) {
    std::vector<uint8_t> b(kVencFrameMetaSize + body, 0);
    b[4] = kFrameCodecH265;
    b[5] = idr ? kFrameFlagIdr : 0;
    for (size_t i = 0; i < body; ++i) {
        b[kVencFrameMetaSize + i] = static_cast<uint8_t>((i * 131u + seed * 7u) & 0xFF);
    }
    return b;
}

FrameFramerConfig framer_cfg(FecScheme scheme, uint16_t i_rate, uint16_t p_rate,
                             uint16_t min_k) {
    FrameFramerConfig c;
    c.stream_type = stream_type::kRtp;
    c.fec.scheme = scheme;
    c.fec.i_rate_permille = i_rate;
    c.fec.p_rate_permille = p_rate;
    c.fec.min_k = min_k;
    return c;
}

// Produce all symbols of one frame at a fixed block_id (framer starts at 0).
std::vector<Sym> produce(FrameFramer& ff, const std::vector<uint8_t>& blob) {
    std::vector<Sym> out;
    ff.on_frame(blob.data(), blob.size(), 1000,
                [&](const uint8_t* f, size_t n, const DataHeader& hdr, uint64_t) {
                    Sym s;
                    s.block_id = hdr.block_id;
                    s.flags = hdr.data_flags;
                    // payload = frame minus the 26-byte DATA header.
                    s.payload.assign(f + kDataHeaderSize, f + n);
                    out.push_back(std::move(s));
                });
    return out;
}

// A no-op emit for drops.
FrameReassembler::Emit noop = [](const uint8_t*, size_t) { return true; };

}  // namespace

int main() {
    FrameReassemblerConfig rc;
    rc.deadline_ms = 50;

    // --- FEC on, no loss: fast path (all sources present) -------------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 250, 100, 3));
        auto blob = make_frame(9000, /*idr=*/true, 1);
        auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        for (const Sym& s : syms) ra.push(s.block_id, s.flags, s.payload.data(),
                                          s.payload.size(), 1000, emit);
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
        CHECK_EQ_U(ra.stats().frames_fast, 1u);
        CHECK_EQ_U(ra.stats().frames_fec, 0u);
    }

    // --- local egress acceptance is the delivery boundary ----------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kNone, 0, 0, 3));
        const auto accepted_blob = make_frame(3000, /*idr=*/true, 40);
        const auto rejected_blob = make_frame(3100, /*idr=*/false, 41);
        FrameReassembler ra(rc);
        std::vector<uint8_t> accepted;
        bool allow = true;
        const FrameReassembler::Emit emit =
            [&](const uint8_t* f, size_t n) {
                if (!allow) return false;
                accepted.assign(f, f + n);
                return true;
            };
        for (const Sym& s : produce(ff, accepted_blob)) {
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000, emit);
        }
        CHECK(accepted == accepted_blob);  // exact bytes, not just length
        allow = false;
        bool completed = false;
        for (const Sym& s : produce(ff, rejected_blob)) {
            completed |= ra.push(s.block_id, s.flags, s.payload.data(),
                                 s.payload.size(), 1001, emit);
        }
        CHECK(completed);  // local refusal still retires packet ARQ
        CHECK(accepted == accepted_blob);  // rejected bytes never replace it
        CHECK_EQ_U(ra.stats().frames_delivered, 1u);
        CHECK_EQ_U(ra.stats().frames_fast, 1u);
        CHECK_EQ_U(ra.stats().frames_egress_rejected, 1u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 0u);
    }

    // --- §3.10 repair feedback is causal and ready after 20 blocks ---------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 250, 100, 3));
        FrameReassembler ra(rc);
        for (uint8_t i = 0; i < 20; ++i) {
            const auto blob = make_frame(3000, false, i);
            for (const Sym& s : produce(ff, blob)) {
                ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                        1000 + i, noop);
            }
        }
        const JsccRepairFeedbackState f = ra.jscc_feedback();
        CHECK(f.have_observation);
        CHECK(f.repair_ready);
        CHECK_EQ_U(f.repair_samples, 20);
        CHECK_EQ_U(f.repair_demand_permille, 0);
        CHECK_EQ_U(f.observed_block_id, 19);
        ra.reset_stats();
        const JsccRepairFeedbackState reset = ra.jscc_feedback();
        CHECK(!reset.have_observation);
        CHECK(!reset.repair_ready);
        CHECK_EQ_U(reset.repair_samples, 0);
    }

    // --- FEC recovery: drop sources, recover from repairs -------------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 300, 100, 3));
        auto blob = make_frame(9000, /*idr=*/true, 2);
        auto syms = produce(ff, blob);
        // Count sources / repairs.
        size_t nsrc = 0, nrep = 0;
        for (const Sym& s : syms)
            ((s.flags & data_flags::kFecRepair) ? nrep : nsrc)++;
        CHECK(nrep >= 2);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        // Drop the first 2 source symbols; feed the rest (incl. repairs).
        size_t dropped = 0;
        for (const Sym& s : syms) {
            const bool is_rep = (s.flags & data_flags::kFecRepair) != 0;
            if (!is_rep && dropped < 2) { ++dropped; continue; }
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000, emit);
        }
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
        CHECK_EQ_U(ra.stats().frames_fec, 1u);
        CHECK_EQ_U(ra.stats().fec_recovered_source_symbols, 2u);
        CHECK_EQ_U(ra.stats().arq_recovered_source_symbols, 0u);
        CHECK_EQ_U(ra.stats().arq_recovered_repair_symbols, 0u);
        CHECK_EQ_U(ra.stats().frames_with_arq, 0u);
        CHECK_EQ_U(ra.stats().frames_fec_only, 1u);
        CHECK_EQ_U(ra.stats().frames_fec_after_arq, 0u);
        CHECK_EQ_U(ra.stats().jscc_observed_repair_symbols, 2u);
        CHECK_EQ_U(ra.stats().jscc_repair_underpredicted_blocks, 1u);
        CHECK_EQ_U(ra.stats().jscc_repair_demand_censored_blocks, 0u);
    }

    // --- ARQ source attribution on the all-source fast path ---------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kNone, 0, 0, 3));
        const auto blob = make_frame(9000, /*idr=*/false, 22);
        const auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        CHECK(syms.size() > 2);
        const Sym& missing = syms.front();
        const Sym& delayed = syms[1];
        for (size_t i = 2; i < syms.size(); ++i) {
            const Sym& s = syms[i];
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000, emit);
        }
        const uint8_t retransmit_flags = static_cast<uint8_t>(
            missing.flags | data_flags::kRetransmit);
        ra.push(missing.block_id, retransmit_flags, missing.payload.data(),
                missing.payload.size(), 1001, emit);
        ra.push(missing.block_id, retransmit_flags, missing.payload.data(),
                missing.payload.size(), 1002, emit);  // duplicate: no count
        CHECK_EQ_U(got.size(), 0u);
        ra.push(delayed.block_id, delayed.flags, delayed.payload.data(),
                delayed.payload.size(), 1003, emit);
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
        CHECK_EQ_U(ra.stats().frames_fast, 1u);
        CHECK_EQ_U(ra.stats().arq_recovered_source_symbols, 1u);
        CHECK_EQ_U(ra.stats().arq_recovered_repair_symbols, 0u);
        CHECK_EQ_U(ra.stats().fec_recovered_source_symbols, 0u);
        CHECK_EQ_U(ra.stats().frames_with_arq, 1u);
        CHECK_EQ_U(ra.stats().frames_fec_only, 0u);
        CHECK_EQ_U(ra.stats().frames_fec_after_arq, 0u);
    }

    // --- FEC completion using an ARQ-retransmitted repair row -------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 300, 100, 3));
        const auto blob = make_frame(9000, /*idr=*/true, 23);
        const auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        const Sym* repair = nullptr;
        bool dropped_source = false;
        for (const Sym& s : syms) {
            if ((s.flags & data_flags::kFecRepair) != 0) {
                if (repair == nullptr) repair = &s;
                continue;
            }
            if (!dropped_source) {
                dropped_source = true;
                continue;
            }
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000, emit);
        }
        CHECK(repair != nullptr);
        ra.push(repair->block_id,
                static_cast<uint8_t>(repair->flags |
                                     data_flags::kRetransmit),
                repair->payload.data(), repair->payload.size(), 1001, emit);
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
        CHECK_EQ_U(ra.stats().frames_fec, 1u);
        CHECK_EQ_U(ra.stats().fec_recovered_source_symbols, 1u);
        CHECK_EQ_U(ra.stats().arq_recovered_source_symbols, 0u);
        CHECK_EQ_U(ra.stats().arq_recovered_repair_symbols, 1u);
        CHECK_EQ_U(ra.stats().frames_with_arq, 1u);
        CHECK_EQ_U(ra.stats().frames_fec_only, 0u);
        CHECK_EQ_U(ra.stats().frames_fec_after_arq, 1u);
    }

    // --- FEC completion after one source row arrives through ARQ ----------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 300, 100, 3));
        const auto blob = make_frame(9000, /*idr=*/true, 24);
        const auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        std::vector<const Sym*> missing;
        const Sym* repair = nullptr;
        for (const Sym& s : syms) {
            if ((s.flags & data_flags::kFecRepair) != 0) {
                if (repair == nullptr) repair = &s;
                continue;
            }
            if (missing.size() < 2) {
                missing.push_back(&s);
                continue;
            }
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000, emit);
        }
        CHECK_EQ_U(missing.size(), 2u);
        CHECK(repair != nullptr);
        ra.push(repair->block_id, repair->flags, repair->payload.data(),
                repair->payload.size(), 1000, emit);
        ra.push(missing[0]->block_id,
                static_cast<uint8_t>(missing[0]->flags |
                                     data_flags::kRetransmit),
                missing[0]->payload.data(), missing[0]->payload.size(), 1001,
                emit);
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
        CHECK_EQ_U(ra.stats().frames_fec, 1u);
        CHECK_EQ_U(ra.stats().fec_recovered_source_symbols, 1u);
        CHECK_EQ_U(ra.stats().arq_recovered_source_symbols, 1u);
        CHECK_EQ_U(ra.stats().arq_recovered_repair_symbols, 0u);
        CHECK_EQ_U(ra.stats().frames_with_arq, 1u);
        CHECK_EQ_U(ra.stats().frames_fec_only, 0u);
        CHECK_EQ_U(ra.stats().frames_fec_after_arq, 1u);
    }

    // --- reorder + duplication (diversity): repairs first, dupes ------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 300, 100, 3));
        auto blob = make_frame(9000, /*idr=*/true, 3);
        auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        // Feed repairs first, then a source dropped, then all sources twice.
        for (const Sym& s : syms)
            if (s.flags & data_flags::kFecRepair)
                ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000, emit);
        bool skipped = false;
        for (const Sym& s : syms) {
            if (s.flags & data_flags::kFecRepair) continue;
            if (!skipped) { skipped = true; continue; }  // drop one source
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000, emit);
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000, emit);  // dup
        }
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == blob);
    }

    // --- unrecoverable: drop more than r; nothing emitted; drop on deadline --
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 100, 100, 3));  // ~10% repair
        auto blob = make_frame(9000, /*idr=*/true, 4);
        auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        // Drop half the source symbols (>> r): unrecoverable.
        size_t si = 0;
        for (const Sym& s : syms) {
            const bool is_rep = (s.flags & data_flags::kFecRepair) != 0;
            if (!is_rep && (si++ % 2 == 0)) continue;  // drop every other source
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 100, emit);
        }
        CHECK_EQ_U(got.size(), 0u);  // NO partial frame
        ra.tick(100 + rc.deadline_ms, noop);  // deadline expires
        CHECK_EQ_U(got.size(), 0u);
        CHECK(ra.stats().frames_deadline >= 1u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 1u);
    }

    // --- conflicting symbol metadata is rejected, never allowed to poison k --
    {
        FrameFramer ff(framer_cfg(FecScheme::kNone, 0, 0, 3));
        auto blob = make_frame(9000, /*idr=*/false, 20);
        auto syms = produce(ff, blob);
        CHECK(syms.size() > 2);

        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };

        ra.push(syms[0].block_id, syms[0].flags, syms[0].payload.data(),
                syms[0].payload.size(), 1000, emit);
        Sym forged = syms[1];
        const uint16_t real_k = be16_read(forged.payload.data());
        be16_write(forged.payload.data(), static_cast<uint16_t>(real_k - 1));
        ra.push(forged.block_id, forged.flags, forged.payload.data(),
                forged.payload.size(), 1000, emit);
        for (size_t i = 1; i < syms.size(); ++i) {
            const Sym& s = syms[i];
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000,
                    emit);
        }
        CHECK_EQ_U(got.size(), 1u);
        CHECK(!got.empty() && got[0] == blob);
        CHECK_EQ_U(ra.stats().malformed, 1u);
    }

    // --- malformed k cannot escape the GF(256) domain or bitmap bounds ------
    {
        FrameReassembler ra(rc);
        std::array<uint8_t, kFecSourceSubheaderSize + 1> source{};
        be16_write(source.data() + kFecSrcOffWindowLen, UINT16_MAX);
        be16_write(source.data() + kFecSrcOffSymIndex, 0);
        ra.push(90, 0, source.data(), source.size(), 1000, noop);

        std::array<uint8_t, kFecRepairSubheaderSize + 1> repair{};
        repair[kFecOffRepairIdx] = 1;  // UINT16_MAX + 1 used to wrap to 0
        be16_write(repair.data() + kFecOffWindowLen, UINT16_MAX);
        be32_write(repair.data() + kFecOffFrameLen, kVencFrameMetaSize);
        ra.push(91, data_flags::kFecRepair, repair.data(), repair.size(),
                1000, noop);

        RepairCandidate cands[2];
        CHECK_EQ_U(ra.repair_candidates(cands, 2), 0);
        CHECK_EQ_U(ra.stats().malformed, 2);
    }

    // --- §14.1 k>256 remains valid on the source-only fast path ------------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 250, 100, 3));
        ff.set_operating_point(0, 0, 58);  // s=21, k=260
        auto blob = make_frame(static_cast<size_t>(21) * 260 -
                                   kVencFrameMetaSize,
                               /*idr=*/true, 44);
        auto syms = produce(ff, blob);
        CHECK_EQ_U(syms.size(), 260);
        CHECK_EQ_U(ff.stats().fec_oversize_k, 1);

        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        for (const Sym& s : syms) {
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000,
                    [&](const uint8_t* f, size_t n) {
                        got.emplace_back(f, f + n);
                        return true;
                    });
        }
        CHECK_EQ_U(got.size(), 1);
        CHECK(!got.empty() && got[0] == blob);
        CHECK_EQ_U(ra.stats().malformed, 0);
    }

    // --- cache completion cannot erase air-path repair demand --------------
    {
        FrameReassembler no_cache(rc);
        FrameReassembler cache_completed(rc);
        const auto push_source = [&](FrameReassembler& ra, uint32_t block,
                                     uint16_t idx, bool air_path) {
            std::array<uint8_t, kFecSourceSubheaderSize + 4> p{};
            be16_write(p.data() + kFecSrcOffWindowLen, 10);
            be16_write(p.data() + kFecSrcOffSymIndex, idx);
            const uint8_t flags =
                idx == 9 ? data_flags::kEndOfBlock : uint8_t{0};
            ra.push(block, flags, p.data(), p.size(), block, noop, air_path);
        };
        for (uint32_t block = 1; block <= 130; ++block) {
            for (uint16_t idx = 0; idx < 9; ++idx) {
                push_source(no_cache, block, idx, true);
                push_source(cache_completed, block, idx, true);
            }
            // Direct cache merge supplies the missing source for delivery,
            // but it remains an air-path loss for the §14.2 estimator.
            push_source(cache_completed, block, 9, false);
        }
        push_source(no_cache, 131, 0, true);  // finalize block 130
        const auto air = no_cache.jscc_feedback();
        const auto cached = cache_completed.jscc_feedback();
        CHECK_EQ_U(air.repair_samples, 120);
        CHECK_EQ_U(cached.repair_samples, 120);
        CHECK_EQ_U(air.repair_demand_permille, 100);
        CHECK_EQ_U(cached.repair_demand_permille, 100);
    }

    // --- conflicting repair frame_len/coded-size cannot corrupt FEC output ---
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 300, 100, 3));
        auto blob = make_frame(9000, /*idr=*/true, 21);
        auto syms = produce(ff, blob);
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };

        bool forged_one = false;
        for (const Sym& s : syms) {
            if (!forged_one && (s.flags & data_flags::kFecRepair) != 0) {
                ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                        1000, emit);
                Sym forged = s;
                be32_write(forged.payload.data() + kFecOffFrameLen,
                           static_cast<uint32_t>(blob.size() - 1));
                ra.push(forged.block_id, forged.flags, forged.payload.data(),
                        forged.payload.size(), 1000, emit);
                forged_one = true;
            }
        }
        for (const Sym& s : syms) {
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 1000,
                    emit);
        }
        CHECK(forged_one);
        CHECK_EQ_U(got.size(), 1u);
        CHECK(!got.empty() && got[0] == blob);
        CHECK_EQ_U(ra.stats().malformed, 1u);
    }

    // --- FEC OFF (ARQ-only): all sources -> deliver; any loss -> nothing -----
    {
        FrameFramer ff(framer_cfg(FecScheme::kNone, 0, 0, 3));
        auto blobA = make_frame(9000, /*idr=*/false, 5);
        auto syms = produce(ff, blobA);
        for (const Sym& s : syms) CHECK((s.flags & data_flags::kFecRepair) == 0);
        // (a) full set -> exact frame.
        {
            FrameReassembler ra(rc);
            std::vector<std::vector<uint8_t>> got;
            auto emit = [&](const uint8_t* f, size_t n) {
                got.emplace_back(f, f + n);
                return true;
            };
            for (const Sym& s : syms) ra.push(s.block_id, s.flags, s.payload.data(),
                                              s.payload.size(), 200, emit);
            CHECK_EQ_U(got.size(), 1u);
            CHECK(got[0] == blobA);
        }
        // (b) drop the FIRST source (leading loss) -> NEVER a corrupt frame.
        {
            FrameReassembler ra(rc);
            std::vector<std::vector<uint8_t>> got;
            auto emit = [&](const uint8_t* f, size_t n) {
                got.emplace_back(f, f + n);
                return true;
            };
            bool dropped_first = false;
            for (const Sym& s : syms) {
                if (!dropped_first) { dropped_first = true; continue; }
                ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(), 200, emit);
            }
            CHECK_EQ_U(got.size(), 0u);
            ra.tick(200 + rc.deadline_ms, noop);
            CHECK_EQ_U(got.size(), 0u);  // leading-loss never mistaken for complete
        }
    }

    // --- multi-frame in order (supersession keeps delivery ordered) ---------
    {
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 250, 100, 3));
        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        std::vector<std::vector<uint8_t>> blobs;
        for (int fr = 0; fr < 4; ++fr) {
            auto blob = make_frame(6000 + fr * 500, fr == 0, static_cast<uint8_t>(10 + fr));
            blobs.push_back(blob);
            for (const Sym& s : produce(ff, blob))
                ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                        1000 + fr, emit);
        }
        CHECK_EQ_U(got.size(), 4u);
        for (int fr = 0; fr < 4; ++fr) CHECK(got[static_cast<size_t>(fr)] == blobs[static_cast<size_t>(fr)]);
    }

    // --- newer available frame supersedes incomplete older frame -----------
    {
        FrameFramer ff(framer_cfg(FecScheme::kNone, 0, 0, 3));
        auto old_blob = make_frame(6000, /*idr=*/false, 30);
        auto new_blob = make_frame(6500, /*idr=*/false, 31);
        auto old_syms = produce(ff, old_blob);
        auto new_syms = produce(ff, new_blob);
        CHECK(old_syms.size() > 1);
        CHECK(new_syms.size() > 1);

        FrameReassembler ra(rc);
        std::vector<std::vector<uint8_t>> got;
        auto emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };
        // Leave the old block incomplete, then release the complete newer one.
        for (size_t i = 1; i < old_syms.size(); ++i) {
            const Sym& s = old_syms[i];
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1000, emit);
        }
        for (const Sym& s : new_syms) {
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1001, emit);
        }
        CHECK_EQ_U(got.size(), 1u);
        CHECK(got[0] == new_blob);
        CHECK_EQ_U(ra.stats().frames_superseded, 1u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 1u);
        CHECK_EQ_U(ra.stats().jscc_shadow_blocks, 2u);
        CHECK_EQ_U(ra.stats().jscc_predicted_loss_symbols, 0u);
        CHECK_EQ_U(ra.stats().jscc_observed_loss_symbols, 0u);
        CHECK_EQ_U(ra.stats().jscc_underpredicted_blocks, 1u);
        CHECK_EQ_U(ra.stats().jscc_repair_demand_censored_blocks, 1u);

        // A late symbol for the finalized old block can never appear after it.
        const Sym& late = old_syms[0];
        ra.push(late.block_id, late.flags, late.payload.data(), late.payload.size(),
                1002, emit);
        CHECK_EQ_U(got.size(), 1u);

        ra.reset_stats();
        CHECK_EQ_U(ra.stats().jscc_shadow_blocks, 0u);
        CHECK_EQ_U(ra.stats().jscc_underpredicted_blocks, 0u);

        // A transmitter restart creates a new session and restarts block IDs.
        // Clearing the stream state must admit block zero again instead of
        // rejecting it behind the previous session's finalized watermark.
        ra.reset_stream();
        FrameFramer rebooted(framer_cfg(FecScheme::kNone, 0, 0, 3));
        auto reboot_blob = make_frame(6200, /*idr=*/true, 32);
        for (const Sym& s : produce(rebooted, reboot_blob)) {
            ra.push(s.block_id, s.flags, s.payload.data(), s.payload.size(),
                    1003, emit);
        }
        CHECK_EQ_U(got.size(), 2u);
        CHECK(got[1] == reboot_blob);
        CHECK_EQ_U(ra.stats().frames_delivered, 1u);
    }

    {
        // §6.3b salvage hook: a block finalizing below k hands its surviving
        // source chunks to the hook (deadline AND supersession), a successful
        // hook emit counts as delivered (not dropped), and the zero-reorder
        // guard blocks a salvage behind a newer emitted block.
        FrameReassemblerConfig rc2;
        rc2.deadline_ms = 50;
        FrameReassembler ra(rc2);
        uint32_t hook_calls = 0;
        uint32_t seen_block = 0;
        uint16_t seen_k = 0;
        uint16_t seen_s = 0;
        size_t seen_sources = 0;
        bool hook_result = true;
        // The view is hook-scoped (the block is erased on return) — copy out
        // what the assertions need.
        ra.set_salvage_hook([&](const SalvageView& v,
                                const FrameReassembler::Emit& emit) {
            ++hook_calls;
            seen_block = v.block_id;
            seen_k = v.k;
            seen_s = v.s;
            seen_sources = v.sources != nullptr ? v.sources->size() : 0;
            if (!hook_result) {
                return false;
            }
            const uint8_t fake[1] = {0x42};
            return emit(fake, 1);
        });
        std::vector<std::vector<uint8_t>> got;
        const FrameReassembler::Emit emit = [&](const uint8_t* f, size_t n) {
            got.emplace_back(f, f + n);
            return true;
        };

        FrameFramer f1(framer_cfg(FecScheme::kRlc256, 250, 100, 1));
        auto blob = make_frame(6000, /*idr=*/false, 9);
        auto syms = produce(f1, blob);  // block 0
        // k sources + 2 repairs; feed all but three so the block stays one
        // symbol below k forever.
        for (size_t i = 0; i + 3 < syms.size(); ++i) {
            ra.push(syms[i].block_id, syms[i].flags, syms[i].payload.data(),
                    syms[i].payload.size(), 1000, emit);
        }
        CHECK_EQ_U(hook_calls, 0u);
        ra.tick(1051, emit);  // deadline -> salvage fires
        CHECK_EQ_U(hook_calls, 1u);
        CHECK_EQ_U(seen_block, 0u);
        CHECK(seen_k > 0 && seen_s > 0);
        CHECK(seen_sources > 0);
        CHECK_EQ_U(got.size(), 1u);
        CHECK_EQ_U(ra.stats().frames_salvaged, 1u);
        CHECK_EQ_U(ra.stats().frames_delivered, 1u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 0u);
        CHECK_EQ_U(ra.stats().frames_deadline, 0u);

        // Supersession path: partial block 1, then block 2 arrives.
        auto blob2 = make_frame(6000, false, 10);
        auto syms2 = produce(f1, blob2);  // block 1
        ra.push(syms2[0].block_id, syms2[0].flags, syms2[0].payload.data(),
                syms2[0].payload.size(), 1060, emit);
        auto blob3 = make_frame(6000, false, 11);
        auto syms3 = produce(f1, blob3);  // block 2
        ra.push(syms3[0].block_id, syms3[0].flags, syms3[0].payload.data(),
                syms3[0].payload.size(), 1061, emit);
        CHECK_EQ_U(hook_calls, 2u);  // block 1 salvaged on supersession
        CHECK_EQ_U(seen_block, 1u);
        CHECK_EQ_U(ra.stats().frames_salvaged, 2u);
        CHECK_EQ_U(ra.stats().frames_superseded, 0u);

        // Hook refusal -> the pre-§6.3b drop accounting.
        hook_result = false;
        ra.tick(1200, emit);  // block 2 hits its deadline
        CHECK_EQ_U(hook_calls, 3u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 1u);
        CHECK_EQ_U(ra.stats().frames_deadline, 1u);
        CHECK_EQ_U(ra.stats().frames_salvaged, 2u);
    }

    // A repaired frame that the local egress refuses is neither a salvage
    // failure nor a radio-unrecoverable frame. It has its own outcome.
    {
        FrameReassembler ra(rc);
        ra.set_salvage_hook([](const SalvageView&,
                               const FrameReassembler::Emit& emit) {
            const uint8_t repaired[2] = {0x12, 0x34};
            return emit(repaired, sizeof(repaired));
        });
        FrameFramer ff(framer_cfg(FecScheme::kRlc256, 250, 100, 1));
        const auto syms = produce(ff, make_frame(6000, false, 42));
        CHECK(syms.size() > 3);
        for (size_t i = 0; i + 3 < syms.size(); ++i) {
            ra.push(syms[i].block_id, syms[i].flags,
                    syms[i].payload.data(), syms[i].payload.size(), 2000,
                    [](const uint8_t*, size_t) { return false; });
        }
        ra.tick(2000 + rc.deadline_ms + 1,
                [](const uint8_t*, size_t) { return false; });
        CHECK_EQ_U(ra.stats().frames_egress_rejected, 1u);
        CHECK_EQ_U(ra.stats().frames_delivered, 0u);
        CHECK_EQ_U(ra.stats().frames_salvaged, 0u);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 0u);
        CHECK_EQ_U(ra.stats().frames_deadline, 0u);
    }

    {
        // No hook set: behaviour identical to pre-§6.3b (drop, count).
        FrameReassemblerConfig rc2;
        rc2.deadline_ms = 50;
        FrameReassembler ra(rc2);
        FrameFramer f1(framer_cfg(FecScheme::kRlc256, 250, 100, 1));
        auto blob = make_frame(6000, false, 9);
        auto syms = produce(f1, blob);
        ra.push(syms[0].block_id, syms[0].flags, syms[0].payload.data(),
                syms[0].payload.size(), 1000, noop);
        ra.tick(1051, noop);
        CHECK_EQ_U(ra.stats().frames_unrecoverable, 1u);
        CHECK_EQ_U(ra.stats().frames_salvaged, 0u);
    }

    return wbtest_finish("frame_reassembler_test");
}
