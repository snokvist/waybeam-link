// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: systematic Cauchy Reed–Solomon FEC codec over GF(256)
// (PROTOCOL.md §14.1). Per-frame block coding — k source symbols + r repair
// symbols, each s bytes — with the MDS guarantee that ANY k of the k+r symbols
// recover the block. Source symbols ship unmodified (zero encode cost on the
// no-loss path); repair coefficient rows are Cauchy and reconstructed at both
// ends from (repair_idx, k) alone, never on the wire. Pure, no floats, no I/O,
// 32-bit-clean for the Android-vendored core. Capacity invariant: k >= 1,
// r >= 0, k + r <= 256.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wblink {

// §14.1 — Cauchy coefficient row for repair symbol repair_idx over k sources:
//   row[i] = 1 / ( x XOR y_i )   in GF(256),
// with y_i = (uint8_t)i (i in 0..k-1) and x = (uint8_t)(k + repair_idx). While
// k + r <= 256 holds, x and every y_i are distinct GF elements, so x XOR y_i is
// never 0 and the inverse is defined. Writes k bytes into row_out. Deterministic
// in (k, repair_idx). Preconditions: k >= 1, k + repair_idx <= 255.
void rlc_repair_row(uint16_t k, uint8_t repair_idx, uint8_t* row_out);

// §14.1 — encode one repair symbol: out[b] = XOR over i of row[i]·sources[i][b].
// sources[i] points at source symbol i (s bytes each); writes s bytes into out.
// Preconditions as for rlc_repair_row; out and every sources[i] hold s bytes.
void rlc_encode_repair(uint16_t k, uint8_t repair_idx,
                       const uint8_t* const* sources, size_t s, uint8_t* out);

// §14.1 — RX decoder: collect any mix of received source/repair symbols and,
// once k distinct ones are held, recover all k source symbols by GF(256)
// Gaussian elimination. The Cauchy structure is MDS, so any k collected symbols
// are linearly independent and recovery is guaranteed. Only the first k distinct
// symbols collected are used; further adds are ignored. Bounds heap to a k×k
// coefficient matrix plus k×s bytes of RHS.
class RlcDecoder {
 public:
    // Precondition: k >= 1.
    RlcDecoder(uint16_t k, size_t s);

    // Received source symbol i (0..k-1); copies s bytes. Ignored if i is out of
    // range, already held, or k distinct symbols are already collected.
    void add_source(uint16_t i, const uint8_t* data);

    // Received repair symbol repair_idx; copies s bytes. Ignored if already held
    // or k distinct symbols are already collected.
    void add_repair(uint8_t repair_idx, const uint8_t* data);

    // Distinct symbols collected so far.
    size_t have() const { return coeffs_.size(); }

    // True once enough symbols are held to recover the block.
    bool can_decode() const { return coeffs_.size() >= k_; }

    // Solve A·S = B for the k source symbols and write them into out
    // (k*s bytes; source symbol i at out + i*s). Returns false if have() < k or
    // a defensive zero-pivot guard trips (never expected under Cauchy-MDS).
    // Idempotent; does not mutate the added symbol data.
    bool decode(uint8_t* out);

 private:
    uint16_t k_;
    size_t s_;
    std::vector<std::vector<uint8_t>> coeffs_;  // one k-byte coeff row per symbol
    std::vector<std::vector<uint8_t>> data_;    // matching s-byte symbol payload
    std::vector<uint8_t> have_source_;          // dedup source index i (size k)
    std::vector<uint8_t> have_repair_;          // dedup repair_idx (size 256)
};

}  // namespace wblink
