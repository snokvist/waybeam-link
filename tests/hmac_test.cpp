// SPDX-License-Identifier: GPL-2.0-or-later
// SHA-256 / HMAC-SHA-256 known-answer tests (FIPS 180-4 examples, RFC 4231
// test cases 1, 2, 6) + the §11.4 csa_mac truncation convention.
#include "wblink/hmac_sha256.h"

#include <cstring>

#include "wbtest.h"

using namespace wblink;

namespace {

bool digest_is(const uint8_t d[32], const char* hex) {
    for (int i = 0; i < 32; ++i) {
        auto nib = [&](char c) -> unsigned {
            return c <= '9' ? static_cast<unsigned>(c - '0')
                            : static_cast<unsigned>(c - 'a' + 10);
        };
        if (d[i] != ((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // --- SHA-256 (FIPS 180-4) ---------------------------------------------
    uint8_t d[32];
    Sha256::digest(reinterpret_cast<const uint8_t*>("abc"), 3, d);
    CHECK(digest_is(
        d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    Sha256::digest(nullptr, 0, d);
    CHECK(digest_is(
        d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    // Two-block message (FIPS example 2).
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256::digest(reinterpret_cast<const uint8_t*>(two), 56, d);
    CHECK(digest_is(
        d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // Incremental == one-shot across an unaligned split.
    {
        Sha256 s;
        s.update(reinterpret_cast<const uint8_t*>(two), 13);
        s.update(reinterpret_cast<const uint8_t*>(two) + 13, 43);
        uint8_t d2[32];
        s.final(d2);
        CHECK(std::memcmp(d, d2, 32) == 0);
    }

    // --- HMAC-SHA-256 (RFC 4231) ------------------------------------------
    // Case 1: key = 20×0x0b, msg = "Hi There".
    {
        uint8_t key[20];
        std::memset(key, 0x0b, sizeof(key));
        hmac_sha256(key, sizeof(key),
                    reinterpret_cast<const uint8_t*>("Hi There"), 8, d);
        CHECK(digest_is(d,
                        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9"
                        "376c2e32cff7"));
    }
    // Case 2: key = "Jefe", msg = "what do ya want for nothing?".
    {
        hmac_sha256(reinterpret_cast<const uint8_t*>("Jefe"), 4,
                    reinterpret_cast<const uint8_t*>(
                        "what do ya want for nothing?"),
                    28, d);
        CHECK(digest_is(d,
                        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec"
                        "58b964ec3843"));
    }
    // Case 6: 131-byte key (> one block, exercises the RFC keying hash),
    // msg = "Test Using Larger Than Block-Size Key - Hash Key First".
    {
        uint8_t key[131];
        std::memset(key, 0xaa, sizeof(key));
        hmac_sha256(key, sizeof(key),
                    reinterpret_cast<const uint8_t*>(
                        "Test Using Larger Than Block-Size Key - Hash Key "
                        "First"),
                    54, d);
        CHECK(digest_is(d,
                        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546"
                        "040f0ee37f54"));
    }

    // --- §11.4 csa_mac: leftmost 4 tag bytes, big-endian --------------------
    {
        uint8_t bytes28[28];
        for (int i = 0; i < 28; ++i) {
            bytes28[i] = static_cast<uint8_t>(i);
        }
        const uint8_t psk[] = {'w', 'b'};
        hmac_sha256(psk, 2, bytes28, 28, d);
        const uint32_t want = (static_cast<uint32_t>(d[0]) << 24) |
                              (static_cast<uint32_t>(d[1]) << 16) |
                              (static_cast<uint32_t>(d[2]) << 8) | d[3];
        CHECK_EQ_U(csa_mac(psk, 2, bytes28), want);
        // Any input bit flips the MAC (smoke, not a crypto claim).
        bytes28[27] ^= 0x01;
        CHECK(csa_mac(psk, 2, bytes28) != want);
    }

    return wbtest_finish("hmac_test");
}
