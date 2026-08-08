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
#   rk3566         needs an aarch64 cross gcc
#   deploy --check needs each config's absolute profile_table on THIS host
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

# Configure THEN build. `cmake --build --preset X` needs the build directory to
# already exist, so a version of this script that only built worked on a
# machine with warm build dirs and failed on a fresh clone — which is exactly
# how CI found it on its first run.
build_preset() {
    run "configure $1" cmake --preset "$1"
    run "build $1"     cmake --build --preset "$1"
}

build_preset dev
run "ctest dev"  ctest --preset dev

if [ "$QUICK" -eq 0 ]; then
    for p in release x86-ground; do
        build_preset "$p"
    done

    # rk3566 is a cross like ssc338q, just one whose compiler happens to be
    # packaged on Debian/Ubuntu. It is guarded for the same reason: a bench
    # host has it, a bare runner does not, and an unguarded gate that only
    # ever runs on the author's machine is not a gate.
    _rk_prefix="${WBLINK_RK3566_PREFIX:-aarch64-linux-gnu-}"
    if command -v "${_rk_prefix}gcc" > /dev/null 2>&1; then
        build_preset rk3566
    else
        skip "rk3566" "no ${_rk_prefix}gcc (apt: g++-aarch64-linux-gnu)"
    fi

    if [ -n "${WBLINK_SSC338Q_TOOLCHAIN:-}" ]; then
        for p in ssc338q ssc338q-au ssc338q-eu; do
            build_preset "$p"
        done
    else
        skip "ssc338q{,-au,-eu}" "set WBLINK_SSC338Q_TOOLCHAIN"
    fi

    if [ -n "${WBLINK_ANDROID_NDK:-}${ANDROID_NDK_HOME:-}${ANDROID_NDK_ROOT:-}" ]; then
        build_preset android-arm64
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
    #
    # Three of the four name an ABSOLUTE profile_table under /etc/waybeam-link,
    # which exists on a bench host and on the nodes themselves and nowhere
    # else. Locally that made this gate pass for the wrong reason — it was
    # testing that the author's machine is a bench host. Guard per config on
    # the table it actually names, and skip loudly when it is missing rather
    # than reporting a pass or a spurious failure.
    for c in deploy/*.json; do
        m=rx; case "$c" in *vehicle*) m=tx;; esac
        tbl=$(sed -n 's/.*"profile_table"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$c" | head -1)
        if [ -n "$tbl" ] && [ "${tbl#/}" != "$tbl" ] && [ ! -f "$tbl" ]; then
            skip "check $(basename "$c")" "profile_table $tbl not on this host"
            continue
        fi
        run "check $(basename "$c")" ./build/dev/waybeam-link "$m" -c "$c" --check
    done

    # config-schema is the only CLI surface no test reaches: app_test
    # suppresses main(), and config_schema_test exercises the function, not
    # the mode. Runs the real binary end to end, which is what proves the
    # argv branch and the write path work at all.
    run "config-schema" ./build/dev/waybeam-link config-schema --json
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
