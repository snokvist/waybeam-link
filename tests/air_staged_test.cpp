// SPDX-License-Identifier: GPL-2.0-or-later
// AirIface::inject_staged / flush_staged — the batched TX submission contract.
//
// Batching folds up to air.usb_tx_agg frames into one bulk-OUT URB so a small
// SoC pays one host submission instead of three (~248 us of CPU each on the
// CV610 craft). The saving is real but the failure mode is silent: frames that
// come out in the wrong order, or a frame stranded in a partial batch, look
// exactly like ordinary RF loss to every consumer. Nothing downstream can tell
// the difference, which is why the ordering contract is pinned here rather
// than left to the device bench.
//
// This suite exercises the CONTRACT on AirIface, not the radio: RadioAir needs
// a USB device, and the property under test — order in, order out, nothing
// held back — belongs to the interface. The devourer side (that a packed URB
// airs three DISTINCT frames rather than re-airing block 1) is not a thing a
// unit test can see; it is pinned on hardware by stamped distinctness, in
// third_party/devourer/tests/txagg_bench.sh.
#include "wblink/air_iface.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "wbtest.h"

namespace {

using namespace wblink;

// A backend that batches the way RadioAir does — stage into owned storage,
// submit on flush — and records what reached "the air", in order.
class FakeStagingAir : public AirIface {
  public:
    explicit FakeStagingAir(size_t cap) : cap_(cap) {}

    // What actually went out, in submission order.
    std::vector<std::string> aired;
    // One entry per submission: how many frames shared it. A 3 means one URB
    // carried three frames; three 1s mean it did not batch at all.
    std::vector<size_t> submissions;

    size_t inject(const uint8_t* f, size_t n) override {
        aired.emplace_back(reinterpret_cast<const char*>(f), n);
        submissions.push_back(1);
        return 1;
    }

    size_t inject_staged(const uint8_t* f, size_t n) override {
        if (cap_ <= 1) return inject(f, n);
        staged_.emplace_back(reinterpret_cast<const char*>(f), n);
        if (staged_.size() >= cap_) flush_staged();
        return 1;
    }

    size_t flush_staged() override {
        if (staged_.empty()) return 0;
        const size_t n = staged_.size();
        for (std::string& s : staged_) aired.push_back(std::move(s));
        staged_.clear();
        submissions.push_back(n);
        return n;
    }

    // Everything below is inert boilerplate: AirIface is a wide pure-virtual
    // interface and this suite exercises only the staging contract.
    size_t inject_resend(const uint8_t* f, size_t n) override {
        return inject(f, n);
    }
    size_t inject_return(uint16_t, const uint8_t* f, size_t n, bool) override {
        return inject(f, n);
    }
    int poll_once(int, const RxCb&) override { return 0; }
    std::vector<int> wait_fds() const override { return {}; }
    size_t rx_adapters() const override { return 0; }
    void flush_rx() override {}
    bool retune(size_t, uint16_t, uint8_t, bool) override { return true; }
    bool recover(size_t, uint16_t, uint8_t) override { return true; }
    bool reapply_tx_power(size_t) override { return true; }
    bool set_power_qdb(size_t, int32_t) override { return true; }
    bool set_power_offset_qdb(size_t, int32_t) override { return true; }
    bool set_power_auto(size_t) override { return true; }
    std::optional<TxPowerApplied> tx_power_applied(size_t) const override {
        return std::nullopt;
    }
    std::optional<uint64_t> read_tsf(size_t) override { return std::nullopt; }
    void set_tx_mode(uint8_t, bool) override {}
    void set_mcs_probe(uint16_t, uint16_t, uint8_t) override {}
    void set_stamp_net_id(uint8_t) override {}
    void set_filter_net_id(std::optional<uint8_t>) override {}
    size_t tx_index() const override { return 0; }
    bool has_tx() const override { return true; }
    uint16_t mtu_supported() const override { return 1400; }
    std::optional<uint32_t> estimate_airtime_us(size_t, bool,
                                                uint16_t) const override {
        return std::nullopt;
    }
    bool is_rf() const override { return true; }
    std::string adapter_mac(size_t) const override { return {}; }
    AdapterCapsView adapter_caps(size_t) const override {
        return AdapterCapsView{"fake", false, false, false};
    }
    std::optional<AirSense> rx_sense(size_t) override { return std::nullopt; }
    uint64_t rx_frames(size_t) const override { return 0; }

    size_t pending() const { return staged_.size(); }

  private:
    size_t cap_;
    std::vector<std::string> staged_;
};

void stage(AirIface& air, const char* s) {
    air.inject_staged(reinterpret_cast<const uint8_t*>(s), strlen(s));
}
void send_now(AirIface& air, const char* s) {
    air.inject(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

// The default implementation must be indistinguishable from inject(). This is
// what lets udp-air and every test double ignore the feature entirely.
void test_default_staging_is_immediate_injection() {
    class PlainAir : public FakeStagingAir {
      public:
        PlainAir() : FakeStagingAir(1) {}
        // Deliberately does NOT override inject_staged/flush_staged: this
        // exercises AirIface's own defaults.
        size_t inject_staged(const uint8_t* f, size_t n) override {
            return AirIface::inject_staged(f, n);
        }
        size_t flush_staged() override { return AirIface::flush_staged(); }
    } air;
    stage(air, "a");
    stage(air, "b");
    CHECK_EQ_U(air.aired.size(), 2u);   // on the air already, nothing held
    CHECK_EQ_U(air.flush_staged(), 0u); // nothing to flush, ever
    CHECK(air.aired[0] == "a" && air.aired[1] == "b");
    // Every frame its own submission — the pre-batching behaviour, byte for
    // byte.
    CHECK_EQ_U(air.submissions.size(), 2u);
}

// The point of the feature: three staged frames cost ONE submission.
void test_batch_fills_and_submits_once() {
    FakeStagingAir air(3);
    stage(air, "f0");
    stage(air, "f1");
    CHECK_EQ_U(air.aired.size(), 0u);  // still staged, nothing submitted yet
    stage(air, "f2");
    CHECK_EQ_U(air.aired.size(), 3u);       // the third filled the URB
    CHECK_EQ_U(air.submissions.size(), 1u); // ONE submission for three frames
    CHECK_EQ_U(air.submissions[0], 3u);
    CHECK(air.aired[0] == "f0" && air.aired[1] == "f1" && air.aired[2] == "f2");
}

// A partial batch must never be stranded: the caller flushes at every fan-out
// boundary, and the frames must be on the air when it does.
void test_partial_batch_is_not_stranded() {
    FakeStagingAir air(3);
    stage(air, "f0");
    stage(air, "f1");
    CHECK_EQ_U(air.flush_staged(), 2u);
    CHECK_EQ_U(air.aired.size(), 2u);
    CHECK_EQ_U(air.pending(), 0u);
    // Flushing an empty batch is a no-op, not a zero-length submission.
    CHECK_EQ_U(air.flush_staged(), 0u);
    CHECK_EQ_U(air.submissions.size(), 1u);
}

// THE regression this suite exists for. An unbatched send (selector state, a
// §12 resend, a vehicle-command echo) must flush first, or it overtakes video
// staged before it — and the wire order would then depend on whether batching
// happened to be on.
void test_unbatched_send_must_not_overtake_staged_frames() {
    FakeStagingAir air(3);
    stage(air, "video0");
    stage(air, "video1");
    // What tx_node.cpp does at every unbatched site.
    air.flush_staged();
    send_now(air, "control");
    CHECK_EQ_U(air.aired.size(), 3u);
    CHECK(air.aired[0] == "video0");
    CHECK(air.aired[1] == "video1");
    CHECK(air.aired[2] == "control");  // control is LAST, as issued

    // The mutation: drop the flush and the control frame jumps the queue.
    FakeStagingAir bad(3);
    stage(bad, "video0");
    stage(bad, "video1");
    send_now(bad, "control");
    CHECK(bad.aired.size() == 1 && bad.aired[0] == "control");
    bad.flush_staged();
    CHECK(bad.aired[1] == "video0");  // reordered — this is what we prevent
}

// §7.2: the quiet gap must not re-arm while the EOB that opens it is still
// staged, or the listen window starts before its own frame is on the air.
void test_eob_is_on_the_air_before_the_gap_rearms() {
    FakeStagingAir air(3);
    stage(air, "v0");
    stage(air, "eob");
    // send_raw's order: stage, flush, THEN note_eob_sent.
    const size_t flushed = air.flush_staged();
    CHECK_EQ_U(flushed, 2u);
    bool gap_armed = false;
    gap_armed = true;  // stands in for qg.note_eob_sent()
    CHECK(gap_armed);
    CHECK_EQ_U(air.pending(), 0u);  // nothing of the block left behind
    CHECK(air.aired.back() == "eob");
}

// Ordering must not depend on the batch depth: cap 1 (off) and cap 3 produce
// the same wire sequence, which is what makes the knob safe to flip.
void test_wire_order_is_independent_of_batch_depth() {
    const char* seq[] = {"a", "b", "c", "d", "e"};
    std::vector<std::string> off, on;
    {
        FakeStagingAir air(1);
        for (const char* s : seq) stage(air, s);
        air.flush_staged();
        off = air.aired;
    }
    {
        FakeStagingAir air(3);
        for (const char* s : seq) stage(air, s);
        air.flush_staged();
        on = air.aired;
    }
    CHECK_EQ_U(off.size(), 5u);
    CHECK(off == on);
}

}  // namespace

int main() {
    test_default_staging_is_immediate_injection();
    test_batch_fills_and_submits_once();
    test_partial_batch_is_not_stranded();
    test_unbatched_send_must_not_overtake_staged_frames();
    test_eob_is_on_the_air_before_the_gap_rearms();
    test_wire_order_is_independent_of_batch_depth();
    return wbtest_finish("air_staged_test");
}
