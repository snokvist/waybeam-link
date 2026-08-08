// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wblink/cookie_stream.h"

namespace wblink {
namespace {

// Byte-for-byte what every call site did before this file existed: one write
// to stderr, flushed. stderr is unbuffered by default, so the fflush is
// belt-and-braces for a consumer that has setvbuf'd it.
void default_write(void*, const char* msg, size_t n) {
    std::fwrite(msg, 1, n, stderr);
    std::fflush(stderr);
}

const LogSink kDefaultSink{&default_write, nullptr};

// One pointer, so fn and cookie can never be read from different generations.
std::atomic<const LogSink*> g_sink{&kDefaultSink};

}  // namespace

void wb_log_set_sink(const LogSink* sink) {
    g_sink.store(sink != nullptr ? sink : &kDefaultSink,
                 std::memory_order_release);
}

void wb_log_write(const char* msg, size_t n) {
    if (n == 0) {
        return;
    }
    const LogSink* s = g_sink.load(std::memory_order_acquire);
    if (s != nullptr && s->fn != nullptr) {
        s->fn(s->cookie, msg, n);
    }
}

void wb_logf(const char* fmt, ...) {
    char stack[512];
    std::va_list ap;
    va_start(ap, fmt);
    const int want = std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (want < 0) {
        return;  // encoding error — nothing sensible to emit
    }
    if (static_cast<size_t>(want) < sizeof(stack)) {
        wb_log_write(stack, static_cast<size_t>(want));
        return;
    }
    // Truncating a diagnostic is how you lose the end of the bus path or the
    // MAC that made it worth logging. Pay for the allocation instead; this is
    // the rare branch.
    std::vector<char> heap(static_cast<size_t>(want) + 1);
    va_start(ap, fmt);
    const int got = std::vsnprintf(heap.data(), heap.size(), fmt, ap);
    va_end(ap);
    if (got > 0) {
        wb_log_write(heap.data(), static_cast<size_t>(got));
    }
}

std::FILE* wb_log_stream() {
    static std::FILE* stream = [] {
        std::FILE* f = wb_fopen_write(nullptr, [](void*, const char* buf,
                                                  size_t n) -> ssize_t {
            wb_log_write(buf, n);
            return static_cast<ssize_t>(n);
        });
        if (f == nullptr) {
            return stderr;
        }
        // Unbuffered, and both reasons are correctness rather than latency.
        //
        // This stream is deliberately never closed, so anything left in a
        // buffer is flushed by the C library's exit-time cleanup — which runs
        // AFTER static destructors, i.e. after a consumer's sink object is
        // gone. Buffered, that is a use-after-free at every process exit.
        // Unbuffered there is never any residue to flush.
        //
        // It also keeps the LogWriteFn contract: a buffered stream hands the
        // sink 8192-byte chunks split mid-line, and log.h promises one
        // complete '\n'-terminated message per call. The construction-time
        // diag stream sets _IOLBF for the same reason (air_radio.cpp); this
        // one goes further because it must also survive teardown ordering.
        std::setvbuf(f, nullptr, _IONBF, 0);
        return f;
    }();
    return stream;
}

}  // namespace wblink
