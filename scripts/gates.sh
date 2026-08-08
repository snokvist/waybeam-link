#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Every merge gate, in one command (issue #143). CI calls this; so should you.
#
# Before this existed the gates were a prose checklist in CLAUDE.md, and two of
# them — the embed gate and the install round trip — exist precisely to catch
# regressions that a normal build cannot see. A `set(... CACHE ... FORCE)` at
# the top of CMakeLists.txt breaks embedding while all eight presets stay
# green; nothing notices unless someone remembers to run the consumer.
#
# Gates needing a toolchain we do not have are SKIPPED, loudly, and counted in
# the summary — a skip must never read as a pass:
#   ssc338q*       needs WBLINK_SSC338Q_TOOLCHAIN
#   android-arm64  needs WBLINK_ANDROID_NDK / ANDROID_NDK_HOME / ANDROID_NDK_ROOT
#
# Hardware trials are NOT here. They need a rig and they radiate; see issue
# #140 and tools/hwtrial_bringup.
#
#   scripts/gates.sh              # everything runnable on this host
#   scripts/gates.sh --quick      # dev build + ctest only
set -u -o pipefail

cd "$(dirname "$0")/.."
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

PASS=(); FAIL=(); SKIP=()
run() {  # run <name> <cmd...>
    local name=$1; shift
    local log; log=$(mktemp)
    if "$@" > "$log" 2>&1; then
        # A build can exit 0 and still have emitted diagnostics for our
        # targets. Vendored trees build under their own flags and are not
        # held to WBLINK_WARNINGS, so they are excluded here, not ignored.
        if grep -E '(warning|error):' "$log" \
           | grep -vE 'third_party|/devourer/|/libusb' | head -5 | grep -q .; then
            echo "FAIL  $name (diagnostics for our targets)"
            grep -E '(warning|error):' "$log" \
              | grep -vE 'third_party|/devourer/|/libusb' | head -5
            FAIL+=("$name")
        else
            echo "ok    $name"
            PASS+=("$name")
        fi
    else
        echo "FAIL  $name"
        tail -15 "$log"
        FAIL+=("$name")
    fi
    rm -f "$log"
}
skip() { echo "SKIP  $1 ($2)"; SKIP+=("$1"); }

run "build dev"  cmake --build --preset dev
run "ctest dev"  ctest --preset dev

if [ "$QUICK" -eq 0 ]; then
    for p in release x86-ground rk3566; do
        run "build $p" cmake --build --preset "$p"
    done

    if [ -n "${WBLINK_SSC338Q_TOOLCHAIN:-}" ]; then
        for p in ssc338q ssc338q-au ssc338q-eu; do
            run "build $p" cmake --build --preset "$p"
        done
    else
        skip "ssc338q{,-au,-eu}" "set WBLINK_SSC338Q_TOOLCHAIN"
    fi

    if [ -n "${WBLINK_ANDROID_NDK:-}${ANDROID_NDK_HOME:-}${ANDROID_NDK_ROOT:-}" ]; then
        run "build android-arm64" cmake --build --preset android-arm64
    else
        skip "android-arm64" "set WBLINK_ANDROID_NDK or ANDROID_NDK_HOME"
    fi

    # B7 embeddability. Its assertions are configure-time, so configuring IS
    # the gate; the build then proves wblink::io actually links.
    run "embed-consumer configure" \
        cmake -S examples/embed-consumer -B build/gates-embed
    run "embed-consumer build" cmake --build build/gates-embed

    # install + find_package round trip, from `release` — never from `dev`,
    # whose archive is ASan-instrumented and whose export carries no
    # -fsanitize usage requirement.
    rm -rf build/gates-stage build/gates-fp
    run "install release" \
        cmake --install build/release --prefix "$PWD/build/gates-stage"
    mkdir -p build/gates-fp-src
    cat > build/gates-fp-src/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(gates_findpkg CXX)
find_package(wblink REQUIRED)
add_executable(fp main.cpp)
target_link_libraries(fp PRIVATE wblink::core)
EOF
    cat > build/gates-fp-src/main.cpp <<'EOF'
#include "wblink/table.h"
int main() { return 0; }
EOF
    run "find_package(wblink)" \
        cmake -S build/gates-fp-src -B build/gates-fp \
              -DCMAKE_PREFIX_PATH="$PWD/build/gates-stage"
    run "find_package build" cmake --build build/gates-fp

    # Deploy-config sanity on the four flying nodes. --check parses and
    # validates; it does NOT prove the config says what you meant (CLAUDE.md).
    for c in deploy/*.json; do
        m=rx; case "$c" in *vehicle*) m=tx;; esac
        run "check $(basename "$c")" ./build/dev/waybeam-link "$m" -c "$c" --check
    done
fi

echo
echo "passed ${#PASS[@]}   failed ${#FAIL[@]}   skipped ${#SKIP[@]}"
if [ "${#SKIP[@]}" -gt 0 ]; then
    echo "skipped: ${SKIP[*]}"
fi
if [ "${#FAIL[@]}" -gt 0 ]; then
    echo "FAILED: ${FAIL[*]}"
    exit 1
fi
