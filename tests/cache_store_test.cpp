// SPDX-License-Identifier: GPL-2.0-or-later
// CacheStore (§14.3) + §3.11 cache-packet codec tests: roundtrips, decode
// validation, retention/eviction, reply subsetting, §13 rate/dedup limits.
#include "wblink/cache_store.h"

#include <cstring>
#include <vector>

#include "wblink/endian.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// Build one source-symbol DATA wire packet (§5.1a subheader + chunk).
std::vector<uint8_t> make_source(uint16_t orig, uint32_t session, uint8_t sid,
                                 uint32_t block, uint16_t k, uint16_t idx,
                                 uint8_t chunk_len, bool eob = false) {
    DataHeader h;
    h.prefix = CommonPrefix{orig, 0, session};
    h.stream_id = sid;
    h.stream_type = stream_type::kRtp;
    h.seq = block * 100 + idx;
    h.block_id = block;
    h.data_flags = eob ? data_flags::kEndOfBlock : 0;
    std::vector<uint8_t> payload(kFecSourceSubheaderSize + chunk_len, 0xAB);
    be16_write(payload.data() + kFecSrcOffWindowLen, k);
    be16_write(payload.data() + kFecSrcOffSymIndex, idx);
    std::vector<uint8_t> out(kDataHeaderSize + payload.size());
    const size_t n = encode_data(h, payload.data(),
                                 static_cast<uint16_t>(payload.size()),
                                 out.data(), out.size());
    out.resize(n);
    return out;
}

// Build one repair-symbol DATA wire packet (§14.1 subheader + s bytes).
std::vector<uint8_t> make_repair(uint16_t orig, uint32_t session, uint8_t sid,
                                 uint32_t block, uint16_t k, uint8_t ridx,
                                 uint8_t s) {
    DataHeader h;
    h.prefix = CommonPrefix{orig, 0, session};
    h.stream_id = sid;
    h.stream_type = stream_type::kRtp;
    h.seq = block * 100 + 50 + ridx;
    h.block_id = block;
    h.data_flags = data_flags::kFecRepair;
    std::vector<uint8_t> payload(kFecRepairSubheaderSize + s, 0xCD);
    payload[kFecOffRepairIdx] = ridx;
    be16_write(payload.data() + kFecOffWindowLen, k);
    be32_write(payload.data() + kFecOffWindowBaseSeq, block * 100);
    be32_write(payload.data() + kFecOffFrameLen, 64);
    std::vector<uint8_t> out(kDataHeaderSize + payload.size());
    const size_t n = encode_data(h, payload.data(),
                                 static_cast<uint16_t>(payload.size()),
                                 out.data(), out.size());
    out.resize(n);
    return out;
}

void feed(CacheStore& store, const std::vector<uint8_t>& frame) {
    const Decoded dec = decode(frame.data(), frame.size());
    const DataView* v = std::get_if<DataView>(&dec);
    CHECK(v != nullptr);
    if (v != nullptr) {
        store.note_data(*v, frame.data(), frame.size());
    }
}

}  // namespace

int main() {
    // --- §3.11 codec roundtrips -------------------------------------------
    {
        CacheStatus s;
        s.prefix = CommonPrefix{33, 9, 0xABCD1234};
        s.target_originator = 17;
        s.target_session = 42;
        s.target_stream_id = 3;
        s.oldest_block = 100;
        s.newest_block = 195;
        s.rx_health_permille = 971;
        s.capability_flags = cache_capability::kIpTransport;
        uint8_t buf[64];
        CHECK_EQ_U(encode_cache_status(s, buf, sizeof(buf)), kCacheStatusSize);
        const Decoded dec = decode(buf, kCacheStatusSize);
        const CacheStatus* rs = std::get_if<CacheStatus>(&dec);
        CHECK(rs != nullptr && *rs == s);
        // health > 1000 and unknown capability bits are decode errors.
        be16_write(buf + 26, 1001);
        CHECK(std::holds_alternative<DecodeError>(decode(buf, kCacheStatusSize)));
        be16_write(buf + 26, 1000);
        buf[28] = 0x02;
        CHECK(std::holds_alternative<DecodeError>(decode(buf, kCacheStatusSize)));
    }
    {
        CacheRequestHeader h;
        h.prefix = CommonPrefix{9, 33, 77};
        h.target_originator = 17;
        h.target_session = 42;
        h.target_stream_id = 3;
        h.target_cache = 33;
        h.request_id = 7;
        h.block_id = 190;
        h.window_len = 10;  // ceil(10/8) = 2 bitmap bytes
        h.max_symbols = 3;
        const uint8_t missing[2] = {0x06, 0x01};  // sources 1,2,8 missing
        const uint8_t have[1] = {0x01};           // repair 0 held
        uint8_t buf[128];
        const size_t n =
            encode_cache_request(h, missing, have, 1, buf, sizeof(buf));
        CHECK_EQ_U(n, kCacheRequestFixedSize + 2 + 1);
        const Decoded dec = decode(buf, n);
        const CacheRequestView* rv = std::get_if<CacheRequestView>(&dec);
        CHECK(rv != nullptr);
        if (rv != nullptr) {
            CHECK(rv->hdr == h);
            CHECK_EQ_U(rv->repair_have_len, 1);
            CHECK_EQ_U(rv->missing_sources[0], 0x06);
            CHECK_EQ_U(rv->missing_sources[1], 0x01);
            CHECK_EQ_U(rv->repair_have[0], 0x01);
        }
        // k = 0 / k > 256 / max_symbols = 0 are encode errors.
        CacheRequestHeader bad = h;
        bad.window_len = 0;
        CHECK_EQ_U(encode_cache_request(bad, missing, nullptr, 0, buf,
                                        sizeof(buf)), 0);
        bad.window_len = 257;
        CHECK_EQ_U(encode_cache_request(bad, missing, nullptr, 0, buf,
                                        sizeof(buf)), 0);
        bad = h;
        bad.max_symbols = 0;
        CHECK_EQ_U(encode_cache_request(bad, missing, nullptr, 0, buf,
                                        sizeof(buf)), 0);
        // Length mismatch on decode (one byte short of the declared shape).
        CHECK(std::holds_alternative<DecodeError>(decode(buf, n - 1)));
    }
    {
        const std::vector<uint8_t> wrapped =
            make_source(17, 42, 3, 190, 10, 2, 32);
        uint8_t buf[2048];
        const size_t n = encode_cache_reply(
            CommonPrefix{33, 9, 0xABCD1234}, 7, wrapped.data(),
            static_cast<uint16_t>(wrapped.size()), buf, sizeof(buf));
        CHECK_EQ_U(n, kCacheReplyFixedSize + wrapped.size());
        const Decoded dec = decode(buf, n);
        const CacheReplyView* rv = std::get_if<CacheReplyView>(&dec);
        CHECK(rv != nullptr);
        if (rv != nullptr) {
            CHECK_EQ_U(rv->request_id, 7);
            CHECK_EQ_U(rv->wrapped_len, wrapped.size());
            CHECK(std::memcmp(rv->wrapped, wrapped.data(), wrapped.size()) ==
                  0);
            // The wrapped bytes revalidate as a DATA packet (§3.11).
            CHECK(std::holds_alternative<DataView>(
                decode(rv->wrapped, rv->wrapped_len)));
        }
        // wrapped_len below a DATA header is invalid.
        uint8_t small[kCacheReplyFixedSize + 4];
        CHECK_EQ_U(encode_cache_reply(CommonPrefix{}, 1, wrapped.data(), 4,
                                      small, sizeof(small)), 0);
    }

    // --- store: retention, status, health ---------------------------------
    CacheStoreConfig cfg;
    cfg.self_originator = 33;
    cfg.stream_ids = {3};
    cfg.blocks = 2;
    cfg.reply_limit = 4;
    cfg.max_requests_per_s = 2;
    CacheStore store(cfg);

    // Block 10: sources 0,1 of k=3 + repair 0. Block 11: all 3 sources.
    feed(store, make_source(17, 42, 3, 10, 3, 0, 16));
    feed(store, make_source(17, 42, 3, 10, 3, 1, 16));
    feed(store, make_repair(17, 42, 3, 10, 3, 0, 16));
    feed(store, make_source(17, 42, 3, 11, 3, 0, 16));
    feed(store, make_source(17, 42, 3, 11, 3, 1, 16));
    feed(store, make_source(17, 42, 3, 11, 3, 2, 16, true));
    // Untracked stream id is ignored.
    feed(store, make_source(17, 42, 5, 10, 3, 0, 16));

    auto st = store.status();
    CHECK_EQ_U(st.size(), 1);
    if (!st.empty()) {
        CHECK_EQ_U(st[0].stream_id, 3);
        CHECK_EQ_U(st[0].oldest_block, 10);
        CHECK_EQ_U(st[0].newest_block, 11);
        CHECK_EQ_U(st[0].rx_health_permille, 1000);  // (3/3 + 3/3 w/repair)/2
        CHECK(st[0].key == (StreamKey{17, 42, 3}));
    }

    // Retention: a third block evicts the oldest (cfg.blocks = 2).
    feed(store, make_source(17, 42, 3, 12, 3, 0, 16));
    st = store.status();
    CHECK_EQ_U(st[0].oldest_block, 11);
    CHECK_EQ_U(st[0].newest_block, 12);

    // --- answer: subsetting + §13 limits -----------------------------------
    CacheRequestHeader req;
    req.prefix = CommonPrefix{9, 33, 77};
    req.target_originator = 17;
    req.target_session = 42;
    req.target_stream_id = 3;
    req.target_cache = 33;
    req.request_id = 1;
    req.block_id = 11;
    req.window_len = 3;
    req.max_symbols = 4;
    const uint8_t missing[1] = {0x06};  // sources 1,2 missing at requester
    CacheRequestView rv;
    rv.hdr = req;
    rv.missing_sources = missing;
    rv.repair_have = nullptr;
    rv.repair_have_len = 0;

    std::vector<const std::vector<uint8_t>*> out;
    CHECK(store.answer(rv, 1000, out) == CacheStore::Verdict::kAnswered);
    CHECK_EQ_U(out.size(), 2);  // sources 1 and 2 held; no repairs for 11

    // Duplicate request_id is silently ignored.
    CHECK(store.answer(rv, 1001, out) == CacheStore::Verdict::kDuplicate);

    // Rate cap: 2/s configured; ids 2 and 3 in the same window.
    rv.hdr.request_id = 2;
    CHECK(store.answer(rv, 1002, out) == CacheStore::Verdict::kAnswered);
    rv.hdr.request_id = 3;
    CHECK(store.answer(rv, 1003, out) == CacheStore::Verdict::kRateLimited);
    // Next 1 s window admits again.
    rv.hdr.request_id = 4;
    CHECK(store.answer(rv, 2100, out) == CacheStore::Verdict::kAnswered);

    // Wrong target cache / unknown stream / out-of-window block.
    rv.hdr.request_id = 5;
    rv.hdr.target_cache = 34;
    CHECK(store.answer(rv, 2101, out) == CacheStore::Verdict::kNotOurs);
    rv.hdr.target_cache = 33;
    rv.hdr.request_id = 6;
    rv.hdr.target_session = 43;
    CHECK(store.answer(rv, 2102, out) == CacheStore::Verdict::kUnknownStream);
    rv.hdr.target_session = 42;
    rv.hdr.request_id = 7;
    rv.hdr.block_id = 10;  // evicted above (fresh 1 s rate window)
    CHECK(store.answer(rv, 3200, out) == CacheStore::Verdict::kNoWindow);

    // max_symbols clamps the reply.
    rv.hdr.request_id = 8;
    rv.hdr.block_id = 11;
    rv.hdr.max_symbols = 1;
    CHECK(store.answer(rv, 3210, out) == CacheStore::Verdict::kAnswered);
    CHECK_EQ_U(out.size(), 1);

    // A new session for the tracked stream replaces the old store.
    feed(store, make_source(17, 99, 3, 500, 3, 0, 16));
    st = store.status();
    CHECK_EQ_U(st.size(), 1);
    CHECK_EQ_U(st[0].oldest_block, 500);
    CHECK(st[0].key == (StreamKey{17, 99, 3}));

    const CacheStoreStats& cs = store.stats();
    CHECK_EQ_U(cs.requests_answered, 4);
    CHECK(cs.symbols_sent >= 5);

    return wbtest_finish("cache_store_test");
}
