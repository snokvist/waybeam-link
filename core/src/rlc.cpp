// SPDX-License-Identifier: GPL-2.0-or-later
// Systematic Cauchy Reed–Solomon codec over GF(256) (PROTOCOL.md §14.1):
// repair-row construction, repair-symbol encode, and MDS Gaussian-elimination
// decode.
#include "wblink/rlc.h"

#include <cstring>

#include "wblink/gf256.h"

namespace wblink {

void rlc_repair_row(uint16_t k, uint8_t repair_idx, uint8_t* row_out) {
    // §14.1 — x = k + repair_idx, y_i = i; row[i] = 1 / (x XOR y_i). k+r<=256
    // keeps x and all y_i distinct, so x XOR y_i != 0 for every i in 0..k-1.
    const uint8_t x = static_cast<uint8_t>(k + repair_idx);
    for (uint16_t i = 0; i < k; ++i) {
        const uint8_t y = static_cast<uint8_t>(i);
        row_out[i] = gf_inv(static_cast<uint8_t>(x ^ y));
    }
}

void rlc_encode_repair(uint16_t k, uint8_t repair_idx,
                       const uint8_t* const* sources, size_t s, uint8_t* out) {
    // §14.1 — out = XOR_i row[i]·source[i]. row fits on the stack (k <= 256).
    uint8_t row[256];
    rlc_repair_row(k, repair_idx, row);
    std::memset(out, 0, s);
    for (uint16_t i = 0; i < k; ++i) {
        const uint8_t c = row[i];
        if (c == 0) {
            continue;  // 0·source[i] contributes nothing (and gf_mul short-circuits)
        }
        const uint8_t* src = sources[i];
        for (size_t b = 0; b < s; ++b) {
            out[b] = static_cast<uint8_t>(out[b] ^ gf_mul(c, src[b]));
        }
    }
}

RlcDecoder::RlcDecoder(uint16_t k, size_t s)
    : k_(k),
      s_(s),
      have_source_(k, 0),
      have_repair_(256, 0) {}

void RlcDecoder::add_source(uint16_t i, const uint8_t* data) {
    if (i >= k_ || have_source_[i] || coeffs_.size() >= k_) {
        return;
    }
    have_source_[i] = 1;
    // §14.1 — a source symbol's coefficient row is the unit vector e_i.
    std::vector<uint8_t> row(k_, 0);
    row[i] = 1;
    coeffs_.push_back(std::move(row));
    data_.emplace_back(data, data + s_);
}

void RlcDecoder::add_repair(uint8_t repair_idx, const uint8_t* data) {
    if (have_repair_[repair_idx] || coeffs_.size() >= k_) {
        return;
    }
    have_repair_[repair_idx] = 1;
    std::vector<uint8_t> row(k_, 0);
    rlc_repair_row(k_, repair_idx, row.data());
    coeffs_.push_back(std::move(row));
    data_.emplace_back(data, data + s_);
}

bool RlcDecoder::decode(uint8_t* out) {
    if (coeffs_.size() < k_) {
        return false;
    }
    const size_t k = k_;
    const size_t s = s_;

    // Flat working copies so the added symbol data is never mutated (idempotent,
    // re-decodable). A is k×k coefficients, B is k×s the RHS symbol payloads.
    std::vector<uint8_t> a(k * k);
    std::vector<uint8_t> b(k * s);
    for (size_t r = 0; r < k; ++r) {
        std::memcpy(&a[r * k], coeffs_[r].data(), k);
        std::memcpy(&b[r * s], data_[r].data(), s);
    }

    // Gauss-Jordan elimination over GF(256). Partial pivoting is not required
    // for a full-rank Cauchy system, but a pivot row must still be found because
    // a unit-vector (source) row need not carry its 1 on the diagonal; a zero
    // pivot with no candidate below is guarded defensively.
    for (size_t col = 0; col < k; ++col) {
        size_t pivot = col;
        while (pivot < k && a[pivot * k + col] == 0) {
            ++pivot;
        }
        if (pivot == k) {
            return false;  // singular — never expected under Cauchy-MDS
        }
        if (pivot != col) {
            for (size_t c = 0; c < k; ++c) {
                std::swap(a[pivot * k + c], a[col * k + c]);
            }
            for (size_t x = 0; x < s; ++x) {
                std::swap(b[pivot * s + x], b[col * s + x]);
            }
        }

        // Normalize the pivot row so a[col][col] == 1.
        const uint8_t inv = gf_inv(a[col * k + col]);
        if (inv != 1) {
            for (size_t c = 0; c < k; ++c) {
                a[col * k + c] = gf_mul(a[col * k + c], inv);
            }
            for (size_t x = 0; x < s; ++x) {
                b[col * s + x] = gf_mul(b[col * s + x], inv);
            }
        }

        // Eliminate this column from every other row.
        for (size_t r = 0; r < k; ++r) {
            if (r == col) {
                continue;
            }
            const uint8_t factor = a[r * k + col];
            if (factor == 0) {
                continue;
            }
            for (size_t c = 0; c < k; ++c) {
                a[r * k + c] = static_cast<uint8_t>(
                    a[r * k + c] ^ gf_mul(factor, a[col * k + c]));
            }
            for (size_t x = 0; x < s; ++x) {
                b[r * s + x] = static_cast<uint8_t>(
                    b[r * s + x] ^ gf_mul(factor, b[col * s + x]));
            }
        }
    }

    // A is now identity; row i of B is source symbol i.
    for (size_t i = 0; i < k; ++i) {
        std::memcpy(out + i * s, &b[i * s], s);
    }
    return true;
}

}  // namespace wblink
