// SPDX-License-Identifier: GPL-2.0-or-later
//
// AirIface — the one contract every frame transport answers.
//
// Before this, app/main.cpp's AirBackend held three concrete backends in
// std::optional and hand-dispatched every call:
//
//     if (mon)   { ... }
//     if (radio) { ... }
//     else       { udp ... }
//
// across ~25 methods and 98 call sites. Some of those methods answer only
// one backend. Each is *documented* as such at its definition
// — this is not a set of typos — but the limitation is invisible where it
// matters: a caller writing `air->recover(...)` in run_rx cannot see that the
// call does nothing on a devourer node, and the consequence is silent (the
// §11.6 watchdog reads "recovery unavailable" as "recovery failed"). The
// original motivating case, the §15.5a scout filter that never widened on
// devourer, landed in Pass 142 — which is the contract working as intended:
// the gap was stated in one place, then closed there.
//
// A pure-virtual contract does not make those backends more capable. It makes
// each backend *state its answer* in one place, where a test can assert it and
// where the answer flips visibly when the capability lands. Every method here
// is pure virtual for that reason, including the ones a given transport has no
// concept of: "udp-air has no TSF" is written down rather than inferred from
// a fall-through.
//
// Divergences that are protocol rulings, not implementation gaps, are tagged
// with their id from docs/devourer-parity-plan.md and left exactly as they
// behave today. This header describes the link as it is.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "wblink/radiotap.h"  // kRxMcsUnknown (AirRxMeta.rx_mcs)

namespace wblink {

// The per-frame RX metadata every backend hands to its callback. It lives
// here rather than beside one transport because it is part of the contract.
struct AirRxMeta {
    uint8_t adapter_id = 0;
    int8_t rssi = 0;      // synthetic on udp-air
    uint64_t tsf_us = 0;  // synthetic on udp-air (loopback has no TSF, §16)
    uint8_t net_id = 0;   // §3.0 L2 tag from the frame SA (0 on udp/loopback)
    uint8_t rx_mcs = kRxMcsUnknown;  // always unknown on udp-air (no PHY)
};

class AirIface {
  public:
    // The RX callback is shared with UdpAir so a backend swap does not
    // re-shape the receive path.
    using RxCb = std::function<void(const AirRxMeta&, const uint8_t*, size_t)>;

    virtual ~AirIface() = default;

    AirIface(const AirIface&) = delete;
    AirIface& operator=(const AirIface&) = delete;

    // ---- frame I/O -------------------------------------------------------
    // Every backend implements these for real; there are no stubs below this
    // heading.

    virtual size_t inject(const uint8_t* frame, size_t len) = 0;

    // §8.4 resend path. Separate from inject() because the radio backends
    // account it separately, not because the wire differs.
    virtual size_t inject_resend(const uint8_t* frame, size_t len) = 0;

    // Returns (NACK / LINK_REPORT) carry their target so an RF backend can
    // address them as §3.0 unicast when return.unicast is on. A transport
    // with no L2 addressing ignores `dest_originator` — and says so.
    virtual size_t inject_return(uint16_t dest_originator,
                                 const uint8_t* frame, size_t len,
                                 bool urgent) = 0;

    virtual int poll_once(int timeout_ms, const RxCb& cb) = 0;

    // Plural because udp-air waits on one socket per adapter. The RF backends
    // return a single-element vector; that shape is the interface's, not a
    // concession to udp.
    virtual std::vector<int> wait_fds() const = 0;

    virtual size_t rx_adapters() const = 0;

    // §11.6 verify hygiene: pre-retune backlog is old-channel residue and must
    // never satisfy a video-verify.
    virtual void flush_rx() = 0;

    // ---- per-adapter radio control ---------------------------------------
    // Indexed like cfg.adapters. Callers loop; the loop lives once, above.

    // `width_mhz` is the canonical unit (20/40/80) for every backend. The
    // devourer backend converts to its own bandwidth code internally — that
    // conversion used to sit in the caller's retune loop, where it was easy
    // to pass a code to a backend expecting MHz.
    virtual bool retune(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz,
                        bool fast) = 0;

    // §11.6 Pass 80 one-shot full re-init after a wedge. G4: on devourer an
    // RX-path restart (Pass 143); a no-op on udp-air.
    virtual bool recover(size_t adapter, uint16_t chan_mhz,
                         uint8_t width_mhz) = 0;

    // §11.2 post-retune TXAGC restore.
    virtual bool reapply_tx_power(size_t adapter) = 0;

    // §10.5: false = the backend did NOT accept the value, and the caller must
    // not cache it as applied.
    virtual bool set_power_qdb(size_t adapter, int32_t qdb) = 0;
    // §10.5 (Pass 150): ONE relative contract — qdb is an offset against this
    // backend's calibrated reference, resolved natively by each backend
    // (devourer: the efuse per-rate table). Returns false when the backend
    // has no reference.
    virtual bool set_power_offset_qdb(size_t adapter, int32_t qdb) = 0;

    // §10.5 auto restore. On devourer "auto" is offset 0, which undoes the
    // latch but leaves any §10.2 curve resolve to re-apply on top.
    virtual bool set_power_auto(size_t adapter) = 0;

    // §10.5 (Pass 169): what the ACTUATOR did with the last write, as opposed
    // to what was asked for. The two diverge because a relative backend folds
    // the offset into a hardware index that rails —
    // effective = clamp(baseline + steps, 0, index_max) — so past the rail
    // every further step commands the same power while the write still
    // succeeds. `qdb` is the offset the chip carries; `saturated_low/high`
    // say the last apply clamped at least one rate at a rail. nullopt on a
    // backend with no power actuator, or before any write.
    //
    // §10.5 (Pass 171): `actuator` is the one field that answers BEFORE any
    // write, because it is a static per-die capability rather than a result.
    // When it is false the other three carry nothing — a chip with no lever
    // answers every offset request with 0, which is byte-identical to a
    // successful zero-offset apply, and reports no rail while having no
    // travel at all. Measured on an RTL8733BU (docs/findings.md 2026-08-14):
    // 18 dB of commanded offset aired nothing, every write reporting success.
    struct TxPowerApplied {
        int32_t qdb = 0;
        bool saturated_low = false;
        bool saturated_high = false;
        bool actuator = true;
    };
    virtual std::optional<TxPowerApplied> tx_power_applied(
        size_t adapter) const = 0;

    // §7.2 TSF-anchored quiet gap.
    virtual std::optional<uint64_t> read_tsf(size_t adapter) = 0;

    // Applies to the TX adapter. Not per-adapter: a node has one uplink.
    virtual void set_tx_mode(uint8_t mcs, bool sgi) = 0;

    // §9.4 Pass 163 sequence-derived rate probe: first-send video DATA
    // frames with seq % period == slot fly candidate_mcs instead of the
    // set_tx_mode() rate. period 0 disarms. Real on the radio backend only
    // (`air.mcs_probe` is refused elsewhere); udp-air no-ops — its frames
    // always fly the committed mode.
    virtual void set_mcs_probe(uint16_t period, uint16_t slot,
                               uint8_t candidate_mcs) = 0;

    // ---- §3.0 identity ---------------------------------------------------

    // Runtime net_id retargeting: the §15.5a scout widens the filter mid-sweep
    // and both roles are re-pinned on selection. Real on devourer (Pass 142);
    // the bench UdpAir has no §3.0 identity to retarget.
    virtual void set_stamp_net_id(uint8_t net_id) = 0;
    virtual void set_filter_net_id(std::optional<uint8_t> net_id) = 0;

    // ---- capability ------------------------------------------------------

    // §15.5a scout: index of the uplink adapter the sweep roams. Resolved from
    // the adapter set on the RF backends. UdpAir answers 0 by convention — it
    // has N virtual adapters but no designated uplink among them.
    virtual size_t tx_index() const = 0;

    // False on an RX-only node, which suppresses the §3.8 heartbeat.
    virtual bool has_tx() const = 0;

    // §9.3a MTU tier. G5: hardcoded per backend rather than probed.
    virtual uint16_t mtu_supported() const = 0;

    // §14.2 JSCC airtime. G2: nullopt where the backend cannot estimate it,
    // which the scheduler already treats as "no airtime signal".
    virtual std::optional<uint32_t> estimate_airtime_us(
        size_t bytes, bool include_pending, uint16_t packet_budget) const = 0;

    // True for a backend that puts frames on real RF. Distinguishes both RF
    // backends from the udp dev transport — it is not "is devourer".
    virtual bool is_rf() const = 0;

    // §10.6 (Pass 154) per-unit adapter identity: the EFUSE MAC as lowercase
    // "aa:bb:cc:dd:ee:ff", read at bring-up on the radio backend. Empty =
    // the backend has no per-unit identity for this adapter — the caller
    // fails closed (no absolute curve), it must never substitute a weaker
    // key. Kernel-monitor answers empty by design: it is deprecated-frozen
    // (ruling #120) and its identity stays the §10.6 monitor tiers.
    virtual std::string adapter_mac(size_t adapter) const = 0;

    // §15.5 (Pass 172): what this die can do, as a stated per-adapter answer.
    // All four fields are static per-die facts read once at bring-up — a
    // capability is an answer on the contract, not a log line an embedder
    // scrapes. `chip` is the backend's chip-generation name ("udp" on the
    // bench backend). `power_actuator` is the §10.5 actuator discriminator
    // (Pass 171): false = every offset is inert and refused. `ldpc_rx_flag`
    // is per-frame LDPC *reporting* (§15.3 Pass 157) — the 8812A decodes
    // LDPC while reporting none, so false taints the stats field, not the
    // link. `fastretune` says the lean retune override exists on this die.
    // No devourer types cross this boundary (the rx_sense rule).
    struct AdapterCapsView {
        std::string chip = "unknown";
        bool power_actuator = false;
        bool ldpc_rx_flag = false;
        bool fastretune = false;
    };
    virtual AdapterCapsView adapter_caps(size_t adapter) const = 0;

    // §15.5a (Pass 155): one adapter's frame-free channel-energy DELTA since
    // the previous read (delta-on-read — a throwaway call is the §15.5a
    // discard barrier). Backend-agnostic mirror of the radio backend's
    // sensor; no devourer types cross this boundary. nullopt = this backend
    // has no frame-free sensor (the udp bench)
    // — §15.5a occupancy then falls back structurally to decoded-frame
    // accounting. Main-thread only (control-plane register I/O).
    struct AirSense {
        bool fa_valid = false;
        uint32_t fa_ofdm = 0;   // false alarms: non-decodable energy events
        uint32_t cca_ofdm = 0;  // channel-busy detections (incl. decodes)
        bool igi_valid = false;
        int igi = 0;            // DIG initial-gain index (floor proxy)
        bool nf_valid = false;
        double nf_dbm = 0.0;    // passive floor: mean rssi − snr over frames
        bool abs_nf_valid = false;
        int8_t abs_nf_dbm = 0;  // absolute idle floor (opt-in generations)
    };
    virtual std::optional<AirSense> rx_sense(size_t adapter) = 0;

    // §11.6 Pass 80 liveness baseline: accepted frames on one adapter. Only
    // this counter is on the contract — the full per-backend counters structs
    // differ by real fields (kernel_dropped/bpf_filtered vs evm/cfo/snr) and
    // reconciling them is a §15.3 schema question, not a dispatch one.
    virtual uint64_t rx_frames(size_t adapter) const = 0;

  protected:
    // Non-copyable, but the concrete backends are returned by value from their
    // create() factories and moved into place, so the base must be movable for
    // a derived `= default` move to compile. Protected: only a backend moves
    // itself, never a caller holding an AirIface.
    AirIface() = default;
    AirIface(AirIface&&) = default;
    AirIface& operator=(AirIface&&) = default;
};

}  // namespace wblink
