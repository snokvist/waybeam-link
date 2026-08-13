// SPDX-License-Identifier: GPL-2.0-or-later
// Discovery and the §15.5a ground scout — the second move of the node/ layer
// (issue #109 Phase 2a).
//
// Both classes came out of `app/main.cpp`'s anonymous namespace verbatim, and
// both were nominated by the extraction plan as clean: neither references any
// other app-layer structure (no AirBackend, no TxCore, no RxCore, no
// UplinkPower). ScoutEngine was already built for this — every side effect it
// has is an injected `Hooks` callback, which is what the rest of Phase 2a is
// working toward for the pieces that are not.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstdint>
#include <cstring>
#include <set>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "wblink/airtime.h"
#include "wblink/csa.h"
#include "wblink/endian.h"
#include "wblink/scout_sense.h"
#include "wblink/scout_store.h"
#include "wblink/wire.h"

namespace wblink {
namespace node {

// Pass 17: bounded, observational catalog. It never feeds latch decisions.
class DiscoveryCatalog {
  public:
    void observe(const Decoded& dec, uint64_t now, uint8_t net_id = 0) {
        const CommonPrefix* p = nullptr;
        const DataView* data = std::get_if<DataView>(&dec);
        const Announce* an = nullptr;
        if (data != nullptr) {
            p = &data->hdr.prefix;
        } else if (const Heartbeat* hb = std::get_if<Heartbeat>(&dec)) {
            p = &hb->prefix;
        } else if ((an = std::get_if<Announce>(&dec)) != nullptr) {
            p = &an->prefix;  // §15.5 presence source (Pass 62)
        } else {
            return;
        }
        const uint64_t nk = (static_cast<uint64_t>(p->originator) << 32) |
                            p->session_id;
        Node& n = nodes_[nk];  // update-in-place keeps prior ANNOUNCE fields
        n.originator = p->originator;
        n.session = p->session_id;
        n.net_id = net_id;
        n.last_seen_ms = now;
        if (an != nullptr) {
            n.announced = true;
            n.claimed = (an->flags & announce_flags::kClaimed) != 0;
            n.claimed_by = an->claimed_by;
            n.psk_present = (an->flags & announce_flags::kPskPresent) != 0;
            if (n.psk_present) {  // Pass 63: announced token is public — cache it
                std::memcpy(n.token.data(), an->psk, kAnnouncePskSize);
                n.have_token = true;
            }
        }
        if (data != nullptr) {
            const uint64_t sk = (nk << 8) | data->hdr.stream_id;
            Stream& s = streams_[sk];
            if (s.packet_count == 0) {
                s.originator = p->originator;
                s.session = p->session_id;
                s.stream_id = data->hdr.stream_id;
                s.stream_type = data->hdr.stream_type;
                s.first_seen_ms = now;
            }
            ++s.packet_count;
            s.last_seen_ms = now;
        }
        trim();
    }

    std::string json(uint64_t now, const std::vector<StreamKey>& latched) {
        age(now);
        std::string out = "{\"nodes\":[";
        bool comma = false;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(n.originator) +
                   ",\"session\":" + std::to_string(n.session) +
                   ",\"net_id\":" + std::to_string(n.net_id) +
                   ",\"last_seen_ms\":" + std::to_string(n.last_seen_ms);
            if (n.announced) {  // §15.5 ANNOUNCE claim view (Pass 62; no token)
                out += ",\"claimed\":" + std::string(n.claimed ? "true" : "false") +
                       ",\"claimed_by\":" + std::to_string(n.claimed_by) +
                       ",\"psk_present\":" +
                       std::string(n.psk_present ? "true" : "false");
            }
            out += "}";
        }
        out += "],\"streams\":[";
        comma = false;
        for (const auto& [key, s] : streams_) {
            (void)key;
            bool is_latched = false;
            for (const StreamKey& k : latched) {
                is_latched = is_latched ||
                             (k.originator == s.originator &&
                              k.session_id == s.session &&
                              k.stream_id == s.stream_id);
            }
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(s.originator) +
                   ",\"session\":" + std::to_string(s.session) +
                   ",\"stream_id\":" + std::to_string(s.stream_id) +
                   ",\"stream_type\":" + std::to_string(s.stream_type) +
                   ",\"packet_count\":" + std::to_string(s.packet_count) +
                   ",\"first_seen_ms\":" + std::to_string(s.first_seen_ms) +
                   ",\"last_seen_ms\":" + std::to_string(s.last_seen_ms) +
                   ",\"latched\":" + (is_latched ? "true" : "false") + "}";
        }
        out += "]}";
        return out;
    }

    // Pass 63: the announced token cached from a craft's ANNOUNCE, for keying a
    // CSA claim (§15.5a). Returns the most-recently-seen token for `originator`.
    std::optional<std::array<uint8_t, kAnnouncePskSize>> token_for(
        uint16_t originator) const {
        std::optional<std::array<uint8_t, kAnnouncePskSize>> best;
        uint64_t best_seen = 0;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (n.originator == originator && n.have_token &&
                n.last_seen_ms >= best_seen) {
                best = n.token;
                best_seen = n.last_seen_ms;
            }
        }
        return best;
    }

    // §15.5a claim staleness (B9): the session `originator` most recently
    // announced. A craft picks a fresh session_id each boot, so a scout
    // candidate whose session differs from the live one predates a reboot —
    // its cached channel is stale. nullopt if the craft was never seen.
    std::optional<uint32_t> session_for(uint16_t originator) const {
        std::optional<uint32_t> best;
        uint64_t best_seen = 0;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (n.originator == originator && n.last_seen_ms >= best_seen) {
                best = n.session;
                best_seen = n.last_seen_ms;
            }
        }
        return best;
    }

  private:
    struct Node {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint8_t net_id = 0;
        uint64_t last_seen_ms = 0;
        bool announced = false;  // §3.12 ANNOUNCE seen (Pass 62)
        bool claimed = false;
        uint16_t claimed_by = 0;
        bool psk_present = false;
        bool have_token = false;  // Pass 63: cached announced token (public)
        std::array<uint8_t, kAnnouncePskSize> token{};
    };
    struct Stream {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint8_t stream_id = 0;
        uint8_t stream_type = 0;
        uint64_t packet_count = 0;
        uint64_t first_seen_ms = 0;
        uint64_t last_seen_ms = 0;
    };
    void age(uint64_t now) {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? nodes_.erase(it)
                                                       : std::next(it);
        }
        for (auto it = streams_.begin(); it != streams_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? streams_.erase(it)
                                                       : std::next(it);
        }
    }
    void trim() {
        while (nodes_.size() > 64) nodes_.erase(nodes_.begin());
        while (streams_.size() > 64) streams_.erase(streams_.begin());
    }
    std::map<uint64_t, Node> nodes_;
    std::map<uint64_t, Stream> streams_;
};

// §15.5a ground scout: one sweep engine. Retunes the scout adapter across a
// channel list, dwelling on each to aggregate the delivered-frame stream into
// per-channel candidates + an occupancy record (ACS-superset; v1 fills only the
// packet-derivable fields). During a sweep the RX filter is widened to hear all
// net_ids; on completion it returns to the resting channel/filter. Runs on the
// single-threaded event loop — no locks. The claim path (quickconnect) is layered
// on top and keys a CsaIssuer from the cached announced token.
class ScoutEngine {
  public:
    struct Hooks {
        std::function<bool(uint16_t chan_mhz, uint8_t bw)> retune;  // scout adapter
        std::function<void(uint16_t chan_mhz, uint8_t bw)> retune_all;  // all ears
        std::function<void(std::optional<uint8_t> net_id)> set_filter;
        std::function<bool(uint16_t originator)> psk_known;
        // §15.5a (Pass 155): frame-free channel sense of one adapter,
        // delta-on-read (the throwaway barrier call and the dwell-end read
        // are the same hook). Null/nullopt = sensor-less backend — the
        // occupancy derivation falls back structurally.
        std::function<std::optional<AirIface::AirSense>(size_t adapter)>
            sense;
        // §15.5a (Pass 161): the scout adapter's calibration-domain key
        // ("mac/<efuse>" else "idx/N") and the node's §3.16 verdict for
        // the one-classifier rule. Null hooks degrade (fixed key, Unknown).
        std::function<std::string()> domain;
        std::function<uint8_t()> verdict;
    };
    ScoutEngine(Hooks h, uint8_t bw, uint16_t rest_chan,
                std::optional<uint8_t> rest_filter, size_t scout_adapter)
        : h_(std::move(h)),
          bw_(bw),
          rest_chan_(rest_chan),
          rest_filter_(rest_filter),
          scout_adapter_(scout_adapter) {}

    // §15.5a (Pass 65): update the channel a sweep returns all ears to — the
    // ground's current operating channel (config default, or a committed target).
    void set_rest_chan(uint16_t chan) { rest_chan_ = chan; }
    void set_rest_filter(std::optional<uint8_t> net_id) {
        rest_filter_ = net_id;
    }
    // A near-tie may prefer the resting channel only when it came from an
    // explicit prior operator selection (persisted preferred_originator or a
    // successful selection in this process). An automatic latch is not enough.
    void set_trusted_rest_originator(std::optional<uint16_t> originator) {
        trusted_rest_originator_ = originator;
    }

    bool scanning() const { return phase_ == Phase::kScanning; }

    // Begin a sweep across `channels` (caller substitutes the allowlist when the
    // request omits them). Returns "" on success or a short error.
    std::string start(std::vector<uint16_t> channels, uint32_t dwell_ms,
                      uint64_t now_ms) {
        if (channels.empty()) return "no channels to scan";
        channels_ = std::move(channels);
        dwell_ms_ = dwell_ms ? dwell_ms : 300;
        // §15.5a (Pass 161): the sweep FOLDS into the store; results_ stays
        // the last sweep's raw occupancy view.
        store_.begin_sweep(h_.domain ? h_.domain() : "idx/0", channels_);
        results_.clear();
        results_folded_ = false;
        chan_idx_ = 0;
        phase_ = Phase::kScanning;
        h_.set_filter(std::nullopt);  // §15.5a: hear all net_ids during a sweep
        enter_channel(now_ms);
        return "";
    }

    void stop(uint64_t now_ms) {
        if (phase_ == Phase::kIdle) return;
        finalize_current(now_ms);
        fold_results();
        rest();
    }

    // Stop without the rest() restore, for a caller that is about to retune
    // and re-pin the filter itself (Pass 144: the claim path). stop() would
    // send every ear back to the resting channel one syscall-per-adapter at a
    // time, only for the claim to move them again a moment later — wasted
    // latency in front of a campaign the craft is timing. The survey for the
    // in-flight channel is still folded in; only the restore is skipped.
    void abandon(uint64_t now_ms) {
        if (phase_ == Phase::kIdle) return;
        finalize_current(now_ms);
        fold_results();
        phase_ = Phase::kIdle;
    }

    // Feed one decoded RX frame (raw wire bytes d/n for length + originator).
    void on_frame(const AirRxMeta& meta, const Decoded& dec, const uint8_t* d,
                  size_t n) {
        // §15.5a (Pass 65): survey only the scout adapter's frames — a diversity
        // ear parked on the resting channel would otherwise smear the craft across
        // every swept channel and inflate occupancy.
        if (phase_ != Phase::kScanning || !channel_ready_ || !barrier_done_ ||
            meta.adapter_id != scout_adapter_ || n < kHeartbeatSize) {
            return;
        }
        const uint16_t originator = be16_read(d + 3);  // §3.1 prefix offset
        ++accum_.frames;
        // §15.5a wifi_util: decodable-frame airtime estimate. Conservative MCS0
        // long-GI, raw PHY; +28 B for the 802.11 header/FCS not in the payload.
        if (const auto us = ht20_service_time_us(n + 28, 0, false, 1000)) {
            accum_.airtime_us += *us;
        }
        if (meta.rssi != 0 && meta.rssi < accum_.min_rssi) {
            accum_.min_rssi = meta.rssi;
        }
        accum_.transmitters.insert(originator);
        ++accum_.frames_by_orig[originator];  // §15.5a (Pass 66): evidence weight
        // RSSI is negative dBm, so "strongest" is the MAXIMUM. 0 stays the
        // no-reading sentinel: an unreported rssi must not read as 0 dBm, the
        // strongest value representable.
        if (meta.rssi != 0) {
            auto [it, fresh] = accum_.best_rssi_by_orig.try_emplace(
                originator, meta.rssi);
            if (!fresh && meta.rssi > it->second) it->second = meta.rssi;
        }
        if (const Announce* an = std::get_if<Announce>(&dec)) {
            Candidate& c = accum_.candidates[originator];
            c.originator = originator;
            c.net_id = meta.net_id;
            c.session = an->prefix.session_id;
            c.claimed = (an->flags & announce_flags::kClaimed) != 0;
            c.claimed_by = an->claimed_by;
            c.psk_known = (an->flags & announce_flags::kPskPresent) != 0 &&
                          h_.psk_known(originator);
            c.chan = channels_[chan_idx_];
        }
    }

    // Advance the sweep when the current dwell elapses. §15.5a (Pass 72): a
    // dwell that heard waybeam frames but resolved no ANNOUNCE candidate yet
    // extends once (up to dwell_ms + kExtendMs total) — the announce cadence
    // (§3.12 ≥1 Hz) can exceed a short base dwell. An extension ends at the
    // first resolved candidate.
    //
    // Craft-finder pacing (findings.md 2026-08-12): the base dwell is now a
    // short presence probe and the extension carries the announce wait. The
    // old shape held a full second on all 38 channels because the dwell is
    // wifi_util's airtime denominator; #173 established no duty cycle is
    // derivable from FA/CCA at all, so that denominator guards a number
    // published duty_cycle_known:false and rendered nowhere. Empty channels
    // stop paying for it; channels with traffic still listen long.
    void tick(uint64_t now_ms) {
        if (phase_ != Phase::kScanning) return;
        // §15.5a (Pass 155): settle elapsed → one throwaway sense read
        // drains the FA/CCA deltas and the frame-quality window; the
        // observe window for the interference denominator starts here.
        // One attempt per dwell; a FAILED drain (USB glitch → nullopt)
        // must NOT arm the observe window — the delta would span back to
        // the last successful drain and read as a saturated interference
        // score on a pristine channel. The dwell falls back sensor-less.
        if (!barrier_done_ && now_ms >= barrier_at_ms_) {
            barrier_done_ = true;
            if (h_.sense && h_.sense(scout_adapter_)) {
                barrier_drained_ = true;
                observe_start_ms_ = now_ms;
            }
        }
        if (now_ms < dwell_deadline_ms_) return;
        // Anything heard extends the dwell once, whether or not a candidate
        // already resolved. This is what lets the base dwell be short: an
        // empty channel costs dwell_ms, and only a channel with waybeam
        // traffic on it pays for the >=1 Hz announce cadence (§3.12).
        //
        // The extension deliberately does NOT stop at the first candidate.
        // Two craft can share a channel, they announce independently, and a
        // dwell that ended on the first ANNOUNCE would silently drop the
        // second — the worst failure available to a page whose whole job is
        // finding craft. Ending early here would have bought ~3 s of a ~12 s
        // sweep and paid for it in missed craft.
        if (!extended_ && accum_.frames > 0) {
            extended_ = true;
            dwell_deadline_ms_ = entered_ms_ + dwell_ms_ + kExtendMs;
            return;
        }
        finalize_current(now_ms);
        if (++chan_idx_ >= channels_.size()) {
            fold_results();
            rest();  // sweep complete
            return;
        }
        enter_channel(now_ms);
    }

    std::string results_json(uint64_t now_ms) const {
        // §15.5a (Pass 161): ranked, explained, accumulated.
        const ScoutRanking rk = store_.rank(
            now_ms, rest_chan_, h_.verdict ? h_.verdict() : 0);
        std::string out = "{\"scanning\":";
        out += scanning() ? "true" : "false";
        out += ",\"current_chan\":";
        out += (scanning() && chan_idx_ < channels_.size())
                   ? std::to_string(channels_[chan_idx_])
                   : "null";
        out += ",\"channels\":[";
        bool comma = false;
        for (const auto& r : results_) {
            if (comma) out += ',';
            comma = true;
            const Occupancy& o = r.occ;
            out += "{\"chan\":" + std::to_string(r.chan) +
                   ",\"tuned\":" + (r.tuned ? "true" : "false") +
                   ",\"evidence_valid\":" +
                   (channel_evidence_valid(r) ? "true" : "false") +
                   ",\"occupancy\":{\"wifi_util_permille\":" +
                   std::to_string(o.wifi_util_permille) +
                   ",\"decoded_airtime_permille\":" +
                   std::to_string(o.wifi_util_permille) +
                   ",\"util_permille\":" + std::to_string(o.util_permille) +
                   ",\"ranking_score_permille\":" +
                   std::to_string(o.util_permille) +
                   ",\"interference_util_permille\":" +
                   (o.have_interference
                        ? std::to_string(o.interference_util_permille)
                        : "null") +
                   ",\"interference_score_permille\":" +
                   (o.have_interference
                        ? std::to_string(o.interference_util_permille)
                        : "null") +
                   ",\"duty_cycle_known\":false" +
                   ",\"noise_dbm\":" +
                   (o.have_noise ? std::to_string(o.noise_dbm) : "null") +
                   ",\"bss_count\":" + std::to_string(o.bss_count) +
                   ",\"quality_permille\":" + std::to_string(o.quality_permille) +
                   ",\"availability_permille\":" +
                   std::to_string(o.availability_permille) + "}}";
        }
        // `candidates` is the consumer-facing list: at most one row per
        // originator and only when channel resolution succeeded. Raw
        // per-dwell observations remain available under candidate_sightings
        // so diagnostics do not turn into multiple selectable craft.
        out += "],\"candidates\":[";
        comma = false;
        std::set<uint16_t> emitted;
        for (const auto& r : results_) {
            for (const auto& c : r.candidates) {
                const auto resolved = candidate_for(c.originator);
                if (!resolved || resolved->chan != c.chan ||
                    !emitted.insert(c.originator).second) {
                    continue;
                }
                if (comma) out += ',';
                comma = true;
                out += "{\"originator\":" + std::to_string(c.originator) +
                       ",\"net_id\":" + std::to_string(c.net_id) +
                       ",\"session\":" + std::to_string(c.session) +
                       ",\"claimed\":" + (c.claimed ? "true" : "false") +
                       ",\"claimed_by\":" + std::to_string(c.claimed_by) +
                       ",\"chan\":" + std::to_string(c.chan) +
                       ",\"frames\":" + std::to_string(c.frames) +
                       ",\"resolved\":true" +
                       ",\"rssi_dbm\":" +
                       (c.rssi_dbm != 0 ? std::to_string(c.rssi_dbm) : "null") +
                       ",\"psk_known\":" + (c.psk_known ? "true" : "false") + "}";
            }
        }
        out += "],\"candidate_sightings\":[";
        comma = false;
        for (const auto& r : results_) {
            for (const auto& c : r.candidates) {
                if (comma) out += ',';
                comma = true;
                const auto resolved = candidate_for(c.originator);
                out += "{\"originator\":" + std::to_string(c.originator) +
                       ",\"net_id\":" + std::to_string(c.net_id) +
                       ",\"session\":" + std::to_string(c.session) +
                       ",\"chan\":" + std::to_string(c.chan) +
                       ",\"frames\":" + std::to_string(c.frames) +
                       ",\"resolved\":" +
                       (resolved && resolved->chan == c.chan ? "true" : "false") +
                       "}";
            }
        }
        // domain is JSON-safe by construction: snprintf %02x MAC or
        // "idx/N" — nothing quotable can appear (pinned here because the
        // invariant lives in air_radio.cpp, three files away).
        out += "],\"ranking\":{\"rounds\":" + std::to_string(rk.rounds) +
               ",\"domain\":\"" + store_.domain() + "\"" +
               ",\"confidence_permille\":" +
               std::to_string(rk.confidence_permille) +
               ",\"rejects\":{\"domain_reset\":" +
               std::to_string(store_.domain_resets()) +
               ",\"implausible\":" +
               std::to_string(store_.rejected_implausible()) +
               ",\"stale\":" + std::to_string(rk.stale_samples) +
               "},\"recommendation\":{\"chan\":" +
               (rk.reason == ScoutRecReason::kOk
                    ? std::to_string(rk.recommended_chan)
                    : std::string("null")) +
               ",\"reason\":\"" + scout_rec_reason_name(rk.reason) +
               "\"},\"bins\":[";
        bool bcomma = false;
        for (const ScoutBinRank& b : rk.bins) {
            if (bcomma) out += ',';
            bcomma = true;
            out += "{\"chan\":" + std::to_string(b.chan_mhz) +
                   ",\"score\":" + std::to_string(b.score) +
                   ",\"burstiness\":" + std::to_string(b.burstiness) +
                   ",\"samples\":" + std::to_string(b.samples) +
                   ",\"qualified\":" + (b.qualified ? "true" : "false") +
                   ",\"age_ms\":" +
                   std::to_string(now_ms >= b.last_seen_ms
                                      ? now_ms - b.last_seen_ms
                                      : 0) +
                   "}";
        }
        out += "]}}";
        return out;
    }

    // §15.5a claim support: what a quickconnect needs about a scouted craft.
    struct Claim {
        uint16_t chan = 0;      // the channel it was heard on most (§15.5a Pass 66)
        uint8_t net_id = 0;     // §3.0 L2 tag to stamp/filter after the claim
        uint32_t session = 0;
        bool psk_known = false; // a usable CSA key is held (token or secret)
    };
    // Best candidate for `originator` across the last sweep. A clearly dominant
    // channel wins; near-equal evidence prefers the resting channel only when
    // an explicit prior selection already pinned that craft there. Otherwise
    // the evidence is ambiguous and selection fails closed.
    std::optional<Claim> candidate_for(uint16_t originator) const {
        // §15.5a (Pass 66): pick the swept channel the craft was heard on with the
        // most frames — robust to a retune-settling leak onto an adjacent channel.
        // >= keeps the last channel on a tie (matches the pre-Pass-66 behaviour).
        std::optional<Claim> found;
        std::optional<Claim> resting;
        uint64_t best_frames = 0;
        uint64_t resting_frames = 0;
        bool saw_competing_channel = false;
        for (const auto& r : results_) {
            for (const auto& c : r.candidates) {
                if (c.originator != originator) continue;
                const Claim claim{c.chan, c.net_id, c.session, c.psk_known};
                if (c.frames >= best_frames) {
                    best_frames = c.frames;
                    found = claim;
                }
                if (c.chan == rest_chan_ && c.frames >= resting_frames) {
                    resting_frames = c.frames;
                    resting = claim;
                } else if (c.chan != rest_chan_) {
                    saw_competing_channel = true;
                }
            }
        }
        // The resting channel needs at least 80% of the top evidence to keep
        // the prior. A real move still wins; a 1166-vs-1137 retune artefact does
        // not. The subtractive form cannot overflow even under a pathological
        // frame counter.
        if (resting && saw_competing_channel &&
            resting_frames >= best_frames - best_frames / 5u) {
            return trusted_rest_originator_ == originator ? resting
                                                        : std::nullopt;
        }
        return found;
    }
    // Emptiest allowlisted channel (lowest wins), skipping `except` (the
    // craft's current channel). §15.5a (Pass 155): ranks on `util_permille`
    // — the interference-inclusive TOTAL — because ranking on decoded
    // airtime alone scored a channel saturated by a non-decodable emitter
    // as pristine. On a sensor-less backend util == wifi_util by
    // construction, so the v1 behaviour is the structural fallback. 0 if
    // no occupancy for any allowed channel (caller then falls back to an
    // explicit target).
    uint16_t emptiest(const std::vector<uint16_t>& allowlist,
                      uint16_t except) const {
        std::vector<ChannelUtil> measured;
        measured.reserve(results_.size());
        for (const auto& r : results_) {
            measured.push_back(ChannelUtil{r.chan, r.occ.util_permille});
        }
        return emptiest_channel(measured, allowlist, except);
    }

  private:
    enum class Phase { kIdle, kScanning };
    struct Occupancy {
        uint16_t wifi_util_permille = 0;
        uint16_t util_permille = 0;
        // §15.5a (Pass 155): frame-free interference index; invalid = JSON
        // null (sensor-less backend / generation without the counter).
        bool have_interference = false;
        uint16_t interference_util_permille = 0;
        bool have_noise = false;
        int noise_dbm = 0;
        uint16_t bss_count = 0;
        uint16_t quality_permille = 0;
        uint16_t availability_permille = 0;
    };
    struct Candidate {
        uint16_t originator = 0;
        uint8_t net_id = 0;
        uint32_t session = 0;
        bool claimed = false;
        uint16_t claimed_by = 0;
        bool psk_known = false;
        uint16_t chan = 0;
        uint64_t frames = 0;  // §15.5a (Pass 66): heard-most channel wins the claim
        // §15.5a: strongest RSSI decoded from this originator on this channel;
        // 0 = none reported (meta.rssi == 0), emitted as JSON null. Strongest,
        // not mean — the consumer question is "which craft is nearest".
        int rssi_dbm = 0;
    };
    struct ChannelResult {
        uint16_t chan = 0;
        bool tuned = true;
        uint64_t at_ms = 0;
        Occupancy occ;
        std::vector<Candidate> candidates;
    };
    struct Accum {
        uint64_t frames = 0;
        uint64_t airtime_us = 0;
        int min_rssi = 0;
        std::set<uint16_t> transmitters;
        std::map<uint16_t, Candidate> candidates;
        std::map<uint16_t, uint64_t> frames_by_orig;  // §15.5a (Pass 66)
        // Strongest RSSI seen per originator this dwell. Keyed like
        // frames_by_orig so a craft heard before its ANNOUNCE still carries
        // its signal into the candidate row.
        std::map<uint16_t, int> best_rssi_by_orig;
    };

    void enter_channel(uint64_t now_ms) {
        accum_ = Accum{};
        channel_ready_ = h_.retune(channels_[chan_idx_], bw_);
        entered_ms_ = now_ms;
        extended_ = false;
        dwell_deadline_ms_ = now_ms + dwell_ms_;
        // §15.5a (Pass 155) dwell hygiene: the discard barrier fires after
        // the settle, from tick() — the delta counters must not charge this
        // bin with its own retune.
        barrier_done_ = false;
        barrier_drained_ = false;
        barrier_at_ms_ = now_ms + kSenseSettleMs;
        observe_start_ms_ = 0;
    }
    void finalize_current(uint64_t now_ms) {
        ChannelResult r;
        r.chan = channels_[chan_idx_];
        r.tuned = channel_ready_;
        r.at_ms = now_ms;
        if (!channel_ready_) {
            results_.push_back(std::move(r));
            return;
        }
        // §15.5a (Pass 72): the actual elapsed dwell is the airtime
        // denominator — an extended (or early-resolved) dwell must not skew
        // wifi_util_permille.
        const uint64_t elapsed_ms =
            now_ms > entered_ms_ ? now_ms - entered_ms_ : dwell_ms_;
        const uint64_t dwell_us = elapsed_ms * 1000u;
        const uint32_t util =
            dwell_us ? static_cast<uint32_t>(std::min<uint64_t>(
                           1000u, accum_.airtime_us * 1000u / dwell_us))
                     : 0u;
        r.occ.wifi_util_permille = static_cast<uint16_t>(util);
        r.occ.bss_count = static_cast<uint16_t>(accum_.transmitters.size());
        // §15.5a (Pass 155): fold the frame-free sensor delta accumulated
        // since the barrier. The observe window is barrier→now; a dwell too
        // short for its barrier reads no sensor and falls back whole.
        std::optional<AirIface::AirSense> sense;
        if (h_.sense && barrier_drained_) {
            sense = h_.sense(scout_adapter_);
        }
        const uint64_t observe_us =
            (barrier_drained_ && now_ms > observe_start_ms_)
                ? (now_ms - observe_start_ms_) * 1000u
                : 0u;
        const OccupancyDerived d = derive_occupancy(
            sense, r.occ.wifi_util_permille, observe_us);
        r.occ.util_permille = d.util_permille;
        r.occ.have_interference = d.interference_valid;
        r.occ.interference_util_permille = d.interference_util_permille;
        r.occ.availability_permille =
            static_cast<uint16_t>(1000u - r.occ.util_permille);
        r.occ.quality_permille = r.occ.availability_permille;  // #100 owns more
        if (d.noise_valid) {
            r.occ.have_noise = true;
            r.occ.noise_dbm = d.noise_dbm;
        } else if (accum_.frames > 0) {
            // Sensor-less fallback: the v1 min-RSSI-of-decoded-frames proxy.
            r.occ.have_noise = true;
            r.occ.noise_dbm = accum_.min_rssi;
        }
        for (auto& [k, c] : accum_.candidates) {
            c.frames = accum_.frames_by_orig[k];  // §15.5a (Pass 66) evidence
            if (const auto it = accum_.best_rssi_by_orig.find(k);
                it != accum_.best_rssi_by_orig.end()) {
                c.rssi_dbm = it->second;
            }
            r.candidates.push_back(c);
        }
        results_.push_back(std::move(r));
    }

    // One Waybeam originator cannot occupy two channels simultaneously. If
    // the same craft decodes under multiple dwell labels, only the resolver's
    // channel is trustworthy; the others prove that channel attribution for
    // those dwells failed. Keep them in raw diagnostics, but never let them
    // contaminate occupancy ranking or look like real craft locations.
    bool channel_evidence_valid(const ChannelResult& r) const {
        if (!r.tuned) return false;
        for (const Candidate& c : r.candidates) {
            const auto resolved = candidate_for(c.originator);
            if (!resolved || resolved->chan != r.chan) return false;
        }
        return true;
    }

    void fold_results() {
        if (results_folded_) return;
        for (const ChannelResult& r : results_) {
            if (!channel_evidence_valid(r)) continue;
            ScoutSample smp;
            smp.chan_mhz = r.chan;
            smp.util_permille = r.occ.util_permille;
            smp.at_ms = r.at_ms;
            store_.fold(smp);
        }
        results_folded_ = true;
    }
    void rest() {
        phase_ = Phase::kIdle;
        h_.set_filter(rest_filter_);     // restore the narrow net_id filter
        h_.retune_all(rest_chan_, bw_);  // §15.5a (Pass 65): return ALL ears to
                                         // rest — heals any prior claim split.
    }

    Hooks h_;
    uint8_t bw_;
    uint16_t rest_chan_;
    std::optional<uint8_t> rest_filter_;
    std::optional<uint16_t> trusted_rest_originator_;
    size_t scout_adapter_;
    Phase phase_ = Phase::kIdle;
    std::vector<uint16_t> channels_;
    uint32_t dwell_ms_ = 300;
    size_t chan_idx_ = 0;
    uint64_t dwell_deadline_ms_ = 0;
    // §15.5a (Pass 72): extension budget past the base dwell — one full
    // worst-case 1 Hz announce period (§3.12) plus margin. Raised 1200->1500
    // (findings.md 2026-08-12) because the craft-finder base dwell shrank to
    // 250 ms: the extension now carries the announce wait almost alone, where
    // it used to sit on top of a full second.
    static constexpr uint64_t kExtendMs = 1500;
    uint64_t entered_ms_ = 0;
    bool extended_ = false;
    // §15.5a (Pass 155) discard-barrier state, reset per channel entry.
    // done = the one attempt was made; drained = it actually returned a
    // value, which is what arms the observe window and the finalize read.
    bool barrier_done_ = false;
    bool barrier_drained_ = false;
    bool channel_ready_ = false;
    uint64_t barrier_at_ms_ = 0;
    uint64_t observe_start_ms_ = 0;
    Accum accum_;
    std::vector<ChannelResult> results_;
    bool results_folded_ = false;
    ScoutStore store_;  // §15.5a Pass 161 accumulated evidence
};

}  // namespace node
}  // namespace wblink
