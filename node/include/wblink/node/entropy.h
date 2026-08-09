// SPDX-License-Identifier: GPL-2.0-or-later
// The node's per-boot randomness (#109 Phase 2c).
//
// `core/` is deliberately RNG-free — it only ever VERIFIES against a supplied
// key — so the two /dev/urandom reads have always lived a layer up. Both have
// the same shape (read, or mix the boot clock and never return all-zero), so
// they move together even though the RX loop reads only the first.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

#include "wblink/node/clock.h"
#include "wblink/types.h"

namespace wblink {
namespace node {

// §2: random per-boot session nonce.
inline uint32_t session_nonce() {
    uint32_t nonce = 0;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(&nonce, 1, sizeof(nonce), f);
        std::fclose(f);
        if (got == sizeof(nonce) && nonce != 0) {
            return nonce;
        }
    }
    return static_cast<uint32_t>(now_ms()) | 1u;  // degraded fallback
}

// §11.4a: per-boot 16-byte announced pairing token P (io/app entropy; the pure
// core layer stays RNG-free and only verifies against a supplied key). Used as
// the craft's CSA HMAC key AND advertised in ANNOUNCE (§3.12) when no operator
// csa.psk is configured (announced mode).
inline std::array<uint8_t, kAnnouncePskSize> announce_token() {
    std::array<uint8_t, kAnnouncePskSize> t{};
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(t.data(), 1, t.size(), f);
        std::fclose(f);
        if (got == t.size()) return t;
    }
    // Degraded fallback: never key with all-zero. Mix the boot clock.
    const uint64_t ms = now_ms();
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<uint8_t>((ms >> ((i % 8) * 8)) ^ (i * 0x9du)) | 1u;
    }
    return t;
}

}  // namespace node
}  // namespace wblink
