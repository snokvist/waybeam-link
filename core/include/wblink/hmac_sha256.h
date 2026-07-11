// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: SHA-256 (FIPS 180-4) + HMAC (RFC 2104) + the §11.4
// csa_mac. Header-only, zero-dep, allocation-free — CSA packets are rare
// (§11.4), so this favors legibility over speed. Not constant-time; the
// only secret it touches is csa_psk on a 4-byte-truncated tag whose spec'd
// threat model is forgery, not local side channels.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wblink {

class Sha256 {
  public:
    static constexpr size_t kDigestLen = 32;
    static constexpr size_t kBlockLen = 64;

    Sha256() { reset(); }

    void reset() {
        static constexpr uint32_t kInit[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, kInit, sizeof(h_));
        len_ = 0;
        buf_used_ = 0;
    }

    void update(const uint8_t* p, size_t n) {
        len_ += n;
        if (buf_used_ != 0) {
            const size_t take =
                n < kBlockLen - buf_used_ ? n : kBlockLen - buf_used_;
            std::memcpy(buf_ + buf_used_, p, take);
            buf_used_ += take;
            p += take;
            n -= take;
            if (buf_used_ == kBlockLen) {
                compress(buf_);
                buf_used_ = 0;
            }
        }
        while (n >= kBlockLen) {
            compress(p);
            p += kBlockLen;
            n -= kBlockLen;
        }
        if (n != 0) {
            std::memcpy(buf_, p, n);
            buf_used_ = n;
        }
    }

    void final(uint8_t out[kDigestLen]) {
        const uint64_t bits = len_ * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0x00;
        while (buf_used_ != 56) {
            update(&zero, 1);
        }
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) {
            lenb[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        update(lenb, 8);
        for (int i = 0; i < 8; ++i) {
            out[4 * i + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

    static void digest(const uint8_t* p, size_t n, uint8_t out[kDigestLen]) {
        Sha256 s;
        s.update(p, n);
        s.final(out);
    }

  private:
    static uint32_t rotr(uint32_t x, unsigned r) {
        return (x >> r) | (x << (32 - r));
    }

    void compress(const uint8_t* p) {
        static constexpr uint32_t kK[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) |
                   (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(p[4 * i + 2]) << 8) |
                   static_cast<uint32_t>(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    uint32_t h_[8];
    uint64_t len_ = 0;
    uint8_t buf_[kBlockLen];
    size_t buf_used_ = 0;
};

// RFC 2104 HMAC-SHA-256. Any key length (longer than one block is hashed
// first, per the RFC).
inline void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        uint8_t out[Sha256::kDigestLen]) {
    uint8_t k[Sha256::kBlockLen] = {0};
    if (key_len > Sha256::kBlockLen) {
        Sha256::digest(key, key_len, k);  // leaves 32 bytes, rest zero
    } else if (key_len != 0) {
        std::memcpy(k, key, key_len);
    }
    uint8_t pad[Sha256::kBlockLen];
    Sha256 inner;
    for (size_t i = 0; i < Sha256::kBlockLen; ++i) {
        pad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
    }
    inner.update(pad, sizeof(pad));
    inner.update(msg, msg_len);
    uint8_t inner_digest[Sha256::kDigestLen];
    inner.final(inner_digest);
    Sha256 outer;
    for (size_t i = 0; i < Sha256::kBlockLen; ++i) {
        pad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    outer.update(pad, sizeof(pad));
    outer.update(inner_digest, sizeof(inner_digest));
    outer.final(out);
}

// §11.4 csa_mac: HMAC-SHA-256 over the encoded CSA packet's bytes 0..27,
// leftmost 4 tag bytes read big-endian.
inline uint32_t csa_mac(const uint8_t* psk, size_t psk_len,
                        const uint8_t* csa_bytes28) {
    uint8_t tag[Sha256::kDigestLen];
    hmac_sha256(psk, psk_len, csa_bytes28, 28, tag);
    return (static_cast<uint32_t>(tag[0]) << 24) |
           (static_cast<uint32_t>(tag[1]) << 16) |
           (static_cast<uint32_t>(tag[2]) << 8) |
           static_cast<uint32_t>(tag[3]);
}

}  // namespace wblink
