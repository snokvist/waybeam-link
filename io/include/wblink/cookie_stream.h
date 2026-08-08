// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A write-only FILE* backed by a caller-supplied callback.
//
// devourer's Logger takes plain FILE* sinks (`set_diag_stream`,
// `EventSink::configure`), and RadioAir feeds it two of them so the machine
// events reach the tx.report harvester and the human diagnostics stay
// volume-bounded (Pass 147) instead of flooding our stdout stats stream.
//
// glibc and musl spell that fopencookie(3). **bionic has neither the function
// nor cookie_io_functions_t** — measured against NDK 26.3.11579264, the pin
// Waybeam-android's :wifi uses: zero hits in the sysroot headers, zero symbols
// in libc.so. What bionic ships is the BSD original, funopen(3), introduced at
// API 24 and so covered by :wifi's minSdk 26.
//
// The callback type below is deliberately (ssize_t, size_t) — the exact
// fopencookie writer signature — so the glibc/musl path is a straight
// pass-through with no wrapper and no allocation, byte-identical to what
// RadioAir did before this header existed. Only bionic pays for the shim,
// because only funopen's writer disagrees (int/int instead of ssize_t/size_t).
//
// This is the repo's first libc-conditional branch, and it is deliberately the
// only one: air_radio.cpp calls wb_fopen_write() and stays platform-clean, and
// core/ is untouched, so its "vendored whole into consumers, dependency-free,
// 32-bit-clean" property (CLAUDE.md) is unaffected.

#include <stdio.h>      // fopencookie / funopen (both are libc extensions)
#include <sys/types.h>  // ssize_t

#include <cstddef>  // size_t
#include <cstdio>   // std::FILE

// Neither spelling is unconditional. glibc gates fopencookie on _GNU_SOURCE
// and bionic gates funopen on __USE_BSD (NDK stdio.h / sys/cdefs.h); both are
// satisfied today only because the compiler predefines _GNU_SOURCE for C++.
// Should that ever stop being true, fail here with a name rather than a
// hundred lines of unrelated template noise at the call site.
#if defined(__BIONIC__) && !defined(__USE_BSD)
#error "cookie_stream.h: funopen not declared (bionic needs _BSD_SOURCE/_GNU_SOURCE before any libc header)"
#endif

namespace wblink {

// Bytes handed to the callback are NOT NUL-terminated. Return the number
// consumed; returning less than `n` is a short write, as with any stdio sink.
using CookieWriteFn = ssize_t (*)(void* cookie, const char* buf, size_t n);

// Returns nullptr if the stream cannot be built. Callers must handle that —
// RadioAir disables the event sink and leaves its tx.report counters at 0
// rather than failing bring-up over a log stream.
inline std::FILE* wb_fopen_write(void* cookie, CookieWriteFn fn) {
#if defined(__BIONIC__)
    // funopen's writer is int(void*, const char*, int), so the cookie has to
    // carry the real one alongside our callback. Freed by the close hook, so
    // fclose() on the returned stream releases it.
    struct Shim {
        void* cookie;
        CookieWriteFn fn;

        static int write(void* c, const char* buf, int n) {
            if (n <= 0) {
                return 0;
            }
            auto* self = static_cast<Shim*>(c);
            const ssize_t done =
                self->fn(self->cookie, buf, static_cast<size_t>(n));
            if (done < 0) {
                return -1;
            }
            return done > n ? n : static_cast<int>(done);
        }
        static int close(void* c) {
            delete static_cast<Shim*>(c);
            return 0;
        }
    };
    auto* shim = new Shim{cookie, fn};
    std::FILE* f = ::funopen(shim, nullptr, &Shim::write, nullptr,
                             &Shim::close);
    if (f == nullptr) {
        delete shim;
    }
    return f;
#else
    cookie_io_functions_t io{};
    io.write = fn;  // exact signature match — no adaptation on glibc/musl
    return ::fopencookie(cookie, "w", io);
#endif
}

}  // namespace wblink
