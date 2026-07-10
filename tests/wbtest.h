// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal zero-dependency check harness (matches devourer's plain-selftest
// style). Each test is a normal executable registered with CTest; a failed
// CHECK prints and sets a nonzero exit code, but the test keeps running so one
// run reports every failure.
#pragma once

#include <cstdio>

namespace wbtest {
inline int failures = 0;
inline int checks = 0;
}  // namespace wbtest

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++wbtest::checks;                                                \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__,   \
                         __LINE__, #cond);                               \
            ++wbtest::failures;                                          \
        }                                                                \
    } while (0)

#define CHECK_EQ_U(a, b)                                                     \
    do {                                                                     \
        ++wbtest::checks;                                                    \
        unsigned long long wbtest_a = static_cast<unsigned long long>(a);    \
        unsigned long long wbtest_b = static_cast<unsigned long long>(b);    \
        if (wbtest_a != wbtest_b) {                                          \
            std::fprintf(stderr,                                            \
                         "CHECK_EQ_U failed %s:%d: %s=%llu != %s=%llu\n",    \
                         __FILE__, __LINE__, #a, wbtest_a, #b, wbtest_b);    \
            ++wbtest::failures;                                             \
        }                                                                    \
    } while (0)

// Return from main(): prints a summary and yields the process exit code.
inline int wbtest_finish(const char* name) {
    std::fprintf(stderr, "%s: %d checks, %d failures\n", name, wbtest::checks,
                 wbtest::failures);
    return wbtest::failures == 0 ? 0 : 1;
}
