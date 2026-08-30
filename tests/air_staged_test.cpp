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
// WHAT IS UNDER TEST: node::StagedAir, the production seam that run_tx routes
// every air write through. Its send_now()/resend() flush before submitting,
// which is what makes an unbatched frame unable to overtake a staged one.
//
// The first version of this file did NOT test that. It defined a fake with
// its own staging and asserted against the fake's behaviour, so deleting all
// ten flush calls from tx_node.cpp left the suite green — verified by doing
// exactly that. The fix was structural: the rule moved into StagedAir, and
// the tests below instantiate that template, so a mutation to the rule fails
// here. The fake is now only a recorder.
//
// Still NOT covered here, and covered on hardware instead: RadioAir's own
// staging (it needs a USB device), and the devourer property that a packed
// URB airs three DISTINCT frames rather than re-airing block 1 — measured by
// stamped distinctness, third_party/devourer/tests/txagg_bench.sh.
#include "wblink/node/staged_air.h"

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
        return AdapterCapsView{"fake", "", "", false, false, false};
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


// ---- StagedAir: the production rule ---------------------------------------
//
// These instantiate node::StagedAir over the recorder above, so the code they
// exercise is the shipped template, not a restatement of it.

using wblink::node::StagedAir;

// THE regression. An unbatched send (selector state, a §12 resend, the §11.7
// echo) must not overtake video staged before it — the wire order must not
// depend on whether batching is on.
//
// MUTATION: delete the flush() from StagedAir::send_now and this fails.
void test_send_now_cannot_overtake_staged_frames() {
    FakeStagingAir air(3);
    StagedAir<FakeStagingAir> sa(air);
    sa.stage(reinterpret_cast<const uint8_t*>("video0"), 6);
    sa.stage(reinterpret_cast<const uint8_t*>("video1"), 6);
    CHECK_EQ_U(air.aired.size(), 0u);  // both still staged
    sa.send_now(reinterpret_cast<const uint8_t*>("control"), 7);
    CHECK_EQ_U(air.aired.size(), 3u);
    CHECK(air.aired[0] == "video0");
    CHECK(air.aired[1] == "video1");
    CHECK(air.aired[2] == "control");  // LAST, as issued
}

// Same rule for the §12 resend path, which is separate on the interface
// because the backends account it separately.
//
// MUTATION: delete the flush() from StagedAir::resend and this fails.
void test_resend_cannot_overtake_staged_frames() {
    FakeStagingAir air(3);
    StagedAir<FakeStagingAir> sa(air);
    sa.stage(reinterpret_cast<const uint8_t*>("v"), 1);
    sa.resend(reinterpret_cast<const uint8_t*>("resend"), 6);
    CHECK_EQ_U(air.aired.size(), 2u);
    CHECK(air.aired[0] == "v");
    CHECK(air.aired[1] == "resend");
}

// §7.2: send_raw stages the EOB, flushes, THEN calls note_eob_sent — so a
// listen window never opens while the frame that opened it is still staged.
// Modelled here as "the EOB is on the air before the observer runs".
void test_eob_is_aired_before_the_gap_observer_runs() {
    FakeStagingAir air(3);
    StagedAir<FakeStagingAir> sa(air);
    sa.stage(reinterpret_cast<const uint8_t*>("v0"), 2);
    sa.stage(reinterpret_cast<const uint8_t*>("eob"), 3);
    size_t aired_when_gap_armed = 0;
    sa.flush();
    aired_when_gap_armed = air.aired.size();  // stands in for note_eob_sent
    CHECK_EQ_U(aired_when_gap_armed, 2u);     // BOTH out before the gap arms
    CHECK(air.aired.back() == "eob");
    CHECK_EQ_U(air.pending(), 0u);
}

// A partial batch is never stranded: the boundary flush submits it.
void test_boundary_flush_submits_a_partial_batch() {
    FakeStagingAir air(3);
    StagedAir<FakeStagingAir> sa(air);
    sa.stage(reinterpret_cast<const uint8_t*>("a"), 1);
    sa.stage(reinterpret_cast<const uint8_t*>("b"), 1);
    CHECK_EQ_U(sa.flush(), 2u);
    CHECK_EQ_U(air.aired.size(), 2u);
    CHECK_EQ_U(sa.flush(), 0u);  // idempotent, not a zero-length submission
    CHECK_EQ_U(air.submissions.size(), 1u);
}

// The knob must not be able to change the wire order. Same sequence through
// StagedAir at cap 1 (off) and cap 3 (on) must air identically.
void test_wire_order_is_independent_of_batch_depth() {
    const char* seq[] = {"a", "b", "c", "d", "e"};
    std::vector<std::string> off, on;
    for (size_t cap : {size_t{1}, size_t{3}}) {
        FakeStagingAir air(cap);
        StagedAir<FakeStagingAir> sa(air);
        for (size_t i = 0; i < 5; ++i) {
            // Interleave an unbatched send, the case most able to reorder.
            if (i == 3) sa.send_now(reinterpret_cast<const uint8_t*>("ctl"), 3);
            sa.stage(reinterpret_cast<const uint8_t*>(seq[i]), 1);
        }
        sa.flush();
        (cap == 1 ? off : on) = air.aired;
    }
    CHECK_EQ_U(off.size(), 6u);
    CHECK(off == on);
}

}  // namespace

int main() {
    test_default_staging_is_immediate_injection();
    test_batch_fills_and_submits_once();
    test_send_now_cannot_overtake_staged_frames();
    test_resend_cannot_overtake_staged_frames();
    test_eob_is_aired_before_the_gap_observer_runs();
    test_boundary_flush_submits_a_partial_batch();
    test_wire_order_is_independent_of_batch_depth();
    return wbtest_finish("air_staged_test");
}
