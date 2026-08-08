// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Where io/'s diagnostics go (B8, issue #144).
//
// Every diagnostic in io/src used to be a bare std::fprintf(stderr, ...). That
// is right for the daemon and wrong for a library: a consumer embedding
// wblink_io — Android's :wifi, the external MonAir RX repo after the #120
// split — has no stderr worth reading, so the adapter that came up on the
// wrong bus path, the calibration artifact keyed on something that shifts on
// re-plug, and the frame-SHM buffer that is wedging ingress all vanish.
//
// The sink is a process-wide function pointer because that is what the call
// sites need and nothing more. io/ has no logger object, no context threaded
// through RadioAir's RX threads, no per-object diagnostics policy — and
// inventing one to carry ~24 line-oriented writes would be a subsystem where a
// pointer does. The default reproduces the old behaviour byte for byte, so the
// daemon and every flying node are unaffected.
//
// NOT covered here: the §15.3 stats line. It is telemetry, not diagnostics,
// and it has its own sink on StatsEmitter (wblink/stats.h) — losing it is a
// different and worse failure than losing a log line.

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace wblink {

// One complete diagnostic message. NOT NUL-terminated; `n` is authoritative.
// Messages arrive '\n'-terminated, because every call site formats it that
// way — a sink that adds its own line break will double-space.
//
// May be called from any thread, including RadioAir's RX loops. Sinks must be
// re-entrant or do their own locking; the default one relies on stdio's.
using LogWriteFn = void (*)(void* cookie, const char* msg, size_t n);

struct LogSink {
    LogWriteFn fn = nullptr;
    void* cookie = nullptr;
};

// Install a sink, or nullptr to restore the stderr default.
//
// The sink is NOT owned and must outlive every subsequent wblink call. It is
// stored as one atomic pointer rather than a pair, so a reader never pairs a
// new callback with the old cookie — the reason for the indirection.
void wb_log_set_sink(const LogSink* sink);

// printf-style, one message per call. Include the trailing '\n'.
// Messages longer than 512 bytes allocate; nothing is truncated.
void wb_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Pre-formatted variant, for buffers that arrive already rendered — devourer
// hands us its diagnostics that way.
void wb_log_write(const char* msg, size_t n);

// A FILE* that forwards into the sink, for the vendored APIs that accept
// nothing else (devourer's Logger::set_diag_stream). Created once per process
// on first use and deliberately never closed: it is the last-resort
// destination for code that logs during teardown, so its lifetime has to
// outlast every object that might. Falls back to stderr if the stream cannot
// be built.
std::FILE* wb_log_stream();

}  // namespace wblink
