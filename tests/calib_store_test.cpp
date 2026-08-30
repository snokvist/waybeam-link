// SPDX-License-Identifier: GPL-2.0-or-later
// §10.6 craft calibration artifact persistence. This store had NO test before
// Pass 195 moved it from one fixed filename per node to one per adapter
// identity, and it is the file that decides what TX power a craft boots at —
// so the round trip, the identity keying, the legacy fallback and the
// filename sanitizer are pinned here.
//
// The headline case is `test_swap_and_return`: it is the regression the Pass
// exists for, and it FAILS against the pre-195 single-file store.
#include "wblink/calib_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {

std::string temp_dir() {
    char tmpl[] = "/tmp/wblink-calXXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Two artifacts whose curves differ in every entry, so "the wrong one was
// loaded" cannot pass by coincidence.
CalibArtifact artifact_with(int32_t base) {
    CalibArtifact a;
    for (size_t i = 0; i < a.curve_qdb.size(); ++i) {
        a.curve_qdb[i] = base + static_cast<int32_t>(i);
        a.placement_qdb[i] = base - static_cast<int32_t>(i);
        a.placement_rssi[i] = static_cast<int8_t>(-50 - static_cast<int>(i));
        a.placement_loss_milli[i] = static_cast<uint16_t>(i);
    }
    return a;
}

const char* kMacA = "mac/aa:bb:cc:dd:ee:01";
const char* kMacB = "mac/aa:bb:cc:dd:ee:02";

std::string artifact_file(const std::string& dir, const std::string& id) {
    return dir + "/artifact-" + calib_identity_slug(id) + ".json";
}

void cleanup(const std::string& dir) {
    for (const char* id : {kMacA, kMacB}) {
        std::remove(artifact_file(dir, id).c_str());
        std::remove((dir + "/curve-" + calib_identity_slug(id) + ".txt").c_str());
    }
    std::remove((dir + "/artifact.json").c_str());
    std::remove((dir + "/curve.txt").c_str());
    std::remove(dir.c_str());
}

// --- the filename rule -----------------------------------------------------
void test_identity_slug() {
    // The live §10.6 form. ':' and '/' are the only characters that actually
    // need mapping today, and both must go — a '/' would put the artifact in
    // a directory that does not exist.
    CHECK(calib_identity_slug("mac/aa:bb:cc:dd:ee:ff") ==
          "mac_aa_bb_cc_dd_ee_ff");
    CHECK(calib_identity_slug("udp") == "udp");
    CHECK(calib_identity_slug("MAC/AA:BB") == "mac_aa_bb");
    // An allowlist, not a blocklist: nothing a future identity tier could
    // contain escapes the artifact directory, and the result never contains a
    // path separator or a traversal component that survives as one.
    const std::string hostile = calib_identity_slug("../../etc/passwd");
    CHECK(hostile.find('/') == std::string::npos);
    CHECK(hostile == ".._.._etc_passwd");
    CHECK(calib_identity_slug("a b\tc") == "a_b_c");
}

// --- round trip ------------------------------------------------------------
void test_round_trip() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    const CalibArtifact a = artifact_with(40);
    const uint8_t fp = calib_store_write(dir, kMacA, a);
    CHECK(fp != 0);  // 0 is the §3.15 "no artifact" sentinel, never a value

    // The filename carries the identity, and the pre-195 fixed name is NOT
    // written — a store that wrote both would keep the overwrite hazard.
    CHECK(exists(artifact_file(dir, kMacA)));
    CHECK(!exists(dir + "/artifact.json"));
    // The operator-readable curve twin follows the same key.
    CHECK(!slurp(dir + "/curve-" + calib_identity_slug(kMacA) + ".txt").empty());

    auto loaded = calib_store_load(dir, kMacA);
    CHECK(static_cast<bool>(loaded));
    if (loaded) {
        CHECK(loaded.value->identity == kMacA);
        CHECK(loaded.value->fingerprint == fp);
        CHECK(loaded.value->curve.valid);
        for (size_t i = 0; i < a.curve_qdb.size(); ++i) {
            CHECK_EQ_U(loaded.value->curve.qdb[i] + 512, a.curve_qdb[i] + 512);
        }
    }
    cleanup(dir);
}

// --- THE REGRESSION --------------------------------------------------------
// Calibrate unit A, swap in unit B and calibrate that, then swap A back. A's
// measurement must still be there. Against the pre-195 single-file store this
// fails: B's write clobbered A, and A came back reading STALE forever with a
// full re-run as the only remedy.
void test_swap_and_return() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    const CalibArtifact a = artifact_with(40);
    const CalibArtifact b = artifact_with(80);
    const uint8_t fp_a = calib_store_write(dir, kMacA, a);
    const uint8_t fp_b = calib_store_write(dir, kMacB, b);
    CHECK(fp_a != 0);
    CHECK(fp_b != 0);

    // Both survive, in the same directory, side by side.
    CHECK(exists(artifact_file(dir, kMacA)));
    CHECK(exists(artifact_file(dir, kMacB)));

    auto back_a = calib_store_load(dir, kMacA);
    CHECK(static_cast<bool>(back_a));
    if (back_a) {
        CHECK(back_a.value->identity == kMacA);
        CHECK(back_a.value->fingerprint == fp_a);
        // A's curve, not B's. Asserted on the VALUES, not just the identity
        // string: a store that returned B's body under A's name would pass an
        // identity-only check and then apply the wrong power curve.
        CHECK_EQ_U(back_a.value->curve.qdb[0] + 512, 40 + 512);
    }

    auto back_b = calib_store_load(dir, kMacB);
    CHECK(static_cast<bool>(back_b));
    if (back_b) {
        CHECK(back_b.value->identity == kMacB);
        CHECK(back_b.value->fingerprint == fp_b);
        CHECK_EQ_U(back_b.value->curve.qdb[0] + 512, 80 + 512);
    }
    cleanup(dir);
}

// --- upgrading a deployed node ---------------------------------------------
// A pre-195 node has one artifact.json. It must keep working with no migration
// step, and the caller's identity check must still be the thing that decides:
// the loader reports the STORED identity and never rewrites it to the one it
// was asked for.
void test_legacy_fallback() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    // Produce a real artifact, then move it to the legacy name to stand in for
    // a file written by the old code. Built by the writer rather than by hand
    // so the body and its fingerprint cannot drift from the format.
    CHECK(calib_store_write(dir, kMacA, artifact_with(40)) != 0);
    const std::string body = slurp(artifact_file(dir, kMacA));
    CHECK(!body.empty());
    std::remove(artifact_file(dir, kMacA).c_str());
    {
        std::ofstream f(dir + "/artifact.json", std::ios::trunc);
        f << body;
    }

    // The unit it was measured on: found through the fallback, applied.
    auto same = calib_store_load(dir, kMacA);
    CHECK(static_cast<bool>(same));
    if (same) {
        CHECK(same.value->identity == kMacA);
        CHECK_EQ_U(same.value->curve.qdb[0] + 512, 40 + 512);
    }

    // A DIFFERENT unit: the legacy file is still returned, carrying the
    // identity it was written under, so the caller's existing match check
    // refuses it exactly as it did before Pass 195. The loader must not
    // silently launder it into a match.
    auto other = calib_store_load(dir, kMacB);
    CHECK(static_cast<bool>(other));
    if (other) {
        CHECK(other.value->identity == kMacA);
        CHECK(other.value->identity != std::string(kMacB));
    }

    // Once this unit is re-calibrated the per-identity file takes precedence,
    // and the stale legacy body is no longer what boots the node.
    CHECK(calib_store_write(dir, kMacB, artifact_with(80)) != 0);
    auto fresh = calib_store_load(dir, kMacB);
    CHECK(static_cast<bool>(fresh));
    if (fresh) {
        CHECK(fresh.value->identity == kMacB);
        CHECK_EQ_U(fresh.value->curve.qdb[0] + 512, 80 + 512);
    }
    cleanup(dir);
}

// --- refusals --------------------------------------------------------------
void test_absent_and_corrupt() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    CHECK(!calib_store_load(dir, kMacA));  // nothing written yet

    CHECK(calib_store_write(dir, kMacA, artifact_with(40)) != 0);
    {
        std::ofstream f(artifact_file(dir, kMacA), std::ios::trunc);
        f << "{\"identity\":";  // truncated mid-object
    }
    // A file that does not parse is refused, NOT silently fallen back to a
    // legacy artifact belonging to some other unit.
    CHECK(!calib_store_load(dir, kMacA));

    // §10.6 D3: an identity-less unit has no artifact to find. The caller
    // refuses before reaching here, but the store must not invent a key.
    CHECK(!calib_store_load(dir, ""));
    cleanup(dir);
}

// A ZERO-LENGTH per-identity artifact — what a failed write actually leaves —
// must NOT fall through to the legacy file. Pre-195 a truncated artifact gave
// "no artifact" and the node stayed on the §10.5 safe boot offset; a fallback
// keyed on emptiness alone would instead resurrect a SUPERSEDED curve for the
// very same unit and install it as a normal boot auto-load.
void test_empty_does_not_resurrect_legacy() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    // A legacy artifact for unit A holding an OLD curve...
    CHECK(calib_store_write(dir, kMacA, artifact_with(40)) != 0);
    const std::string old_body = slurp(artifact_file(dir, kMacA));
    {
        std::ofstream f(dir + "/artifact.json", std::ios::trunc);
        f << old_body;
    }
    // ...and a newer per-identity one for the same unit, then truncated to
    // zero bytes by a write that ran out of space.
    CHECK(calib_store_write(dir, kMacA, artifact_with(88)) != 0);
    {
        std::ofstream f(artifact_file(dir, kMacA), std::ios::trunc);
    }
    CHECK(slurp(artifact_file(dir, kMacA)).empty());

    auto loaded = calib_store_load(dir, kMacA);
    CHECK(!loaded);  // refused — NOT the 40-curve legacy body
    if (loaded) {
        // Name the actual failure rather than just the count: silently
        // applying qdb[0]=40 here is the bug, and a bare CHECK would not say
        // which curve came back.
        std::fprintf(stderr, "  resurrected curve qdb[0]=%d\n",
                     loaded.value->curve.qdb[0]);
    }
    // Remove the empty file and the legacy one IS reachable again — the
    // refusal is about an unusable file being present, not a permanent veto.
    std::remove(artifact_file(dir, kMacA).c_str());
    auto again = calib_store_load(dir, kMacA);
    CHECK(static_cast<bool>(again));
    if (again) CHECK_EQ_U(again.value->curve.qdb[0] + 512, 40 + 512);
    cleanup(dir);
}

}  // namespace

int main() {
    test_identity_slug();
    test_round_trip();
    test_swap_and_return();
    test_legacy_fallback();
    test_absent_and_corrupt();
    test_empty_does_not_resurrect_legacy();
    return wbtest_finish("calib_store");
}
