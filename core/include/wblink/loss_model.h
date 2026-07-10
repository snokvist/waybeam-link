// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: synthetic loss models for the bench (PROTOCOL.md §16.2).
//
// Deterministic (seeded xorshift, no <random>, no Date/clock): the same seed
// reproduces the same loss pattern on every platform. Models: uniform p and
// Gilbert-Elliott burst (the ARQ target regime). AdapterLossField instantiates
// one model per virtual adapter (independent losses = the diversity case) plus
// a shared model mixed in by a correlation knob rho — rho→1 approximates the
// correlated all-adapter fade that §14/§17-gate-2 cares about.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace wblink {

struct LossRng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    // [0, 1)
    double uniform() {
        return static_cast<double>(next() >> 11) *
               (1.0 / 9007199254740992.0);
    }
};

struct GeParams {
    double p_gb = 0.01;  // good -> bad transition probability per packet
    double p_bg = 0.2;   // bad -> good
    double loss_g = 0.0;
    double loss_b = 0.8;
};

class GeLossModel {
  public:
    explicit GeLossModel(const GeParams& p) : p_(p) {}
    bool drop(LossRng& rng) {
        if (bad_) {
            if (rng.uniform() < p_.p_bg) {
                bad_ = false;
            }
        } else {
            if (rng.uniform() < p_.p_gb) {
                bad_ = true;
            }
        }
        return rng.uniform() < (bad_ ? p_.loss_b : p_.loss_g);
    }
    bool bad() const { return bad_; }

  private:
    GeParams p_;
    bool bad_ = false;
};

class AdapterLossField {
  public:
    AdapterLossField(uint8_t adapters, uint64_t seed, double correlation,
                     double uniform_p, std::optional<GeParams> ge)
        : correlation_(correlation), uniform_p_(uniform_p) {
        mix_rng_.s = seed ^ 0xA5A5A5A5A5A5A5A5ull;
        shared_ = make_model(seed ^ 0x517CC1B727220A95ull, ge);
        for (uint8_t i = 0; i < adapters; ++i) {
            per_.push_back(make_model(
                seed + 0x9E3779B97F4A7C15ull * (i + 1u), ge));
        }
    }

    // Call once per air packet, before the per-adapter drop() calls.
    void begin_packet() { shared_drop_ = model_drop(shared_); }

    // Drop verdict for this packet on this adapter.
    bool drop(uint8_t adapter) {
        Model& m = per_[adapter % per_.size()];
        const bool own = model_drop(m);  // always advance (keeps GE state)
        if (correlation_ > 0.0 && mix_rng_.uniform() < correlation_) {
            return shared_drop_;
        }
        return own;
    }

    uint8_t adapters() const { return static_cast<uint8_t>(per_.size()); }

  private:
    struct Model {
        LossRng rng;
        std::optional<GeLossModel> ge;
    };

    Model make_model(uint64_t seed, const std::optional<GeParams>& ge) {
        Model m;
        m.rng.s = seed | 1u;
        if (ge) {
            m.ge.emplace(*ge);
        }
        return m;
    }

    bool model_drop(Model& m) {
        if (m.ge) {
            return m.ge->drop(m.rng);
        }
        return m.rng.uniform() < uniform_p_;
    }

    double correlation_;
    double uniform_p_;
    LossRng mix_rng_;
    Model shared_;
    std::vector<Model> per_;
    bool shared_drop_ = false;
};

}  // namespace wblink
