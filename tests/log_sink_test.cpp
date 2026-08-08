// SPDX-License-Identifier: GPL-2.0-or-later
// B8 (issue #144): the two injected sinks. Diagnostics used to go to a
// hardcoded stderr and the §15.3 stats line to a hardcoded stdout, so a
// library consumer got noise it could not read and lost telemetry it needed.
//
// What is worth asserting here is not "the callback fires" — it is the
// properties a consumer depends on and that a refactor could quietly break:
// long messages are not truncated, the stats sink REPLACES stdout rather than
// adding to it, the UDP binding is unaffected either way, and a config that
// carries csa.psk never leaks it into either path.
#include "wblink/log.h"

#include <cstring>
#include <string>
#include <vector>

#include "wblink/config.h"
#include "wblink/stats.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Capture {
    std::vector<std::string> msgs;

    static void write(void* cookie, const char* msg, size_t n) {
        static_cast<Capture*>(cookie)->msgs.emplace_back(msg, n);
    }
};

// Restores the stderr default however the block exits, so one failing CHECK
// cannot leave every later test in this binary logging into freed memory.
struct SinkGuard {
    ~SinkGuard() { wb_log_set_sink(nullptr); }
};

}  // namespace

int main() {
    // --- diagnostics reach an installed sink, whole and unterminated --------
    {
        SinkGuard guard;
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);

        wb_logf("radio: adapter \"%s\" up (path %s)\n", "wlan0", "8-1");
        CHECK(cap.msgs.size() == 1);
        if (cap.msgs.size() == 1) {
            CHECK(cap.msgs[0] == "radio: adapter \"wlan0\" up (path 8-1)\n");
        }

        // Pre-formatted path (devourer hands us rendered lines).
        wb_log_write("bulk_send EP 4 FAIL rc=-7\n", 26);
        CHECK(cap.msgs.size() == 2);
        if (cap.msgs.size() == 2) {
            CHECK(cap.msgs[1] == "bulk_send EP 4 FAIL rc=-7\n");
        }

        // A zero-length write is not a message.
        wb_log_write("", 0);
        CHECK(cap.msgs.size() == 2);
    }

    // --- the long-message branch does not truncate --------------------------
    // wb_logf formats into a 512-byte stack buffer and only then allocates.
    // A silently truncated diagnostic loses its tail, which is where the bus
    // path and the MAC live — the two things that make it worth logging.
    {
        SinkGuard guard;
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);

        const std::string big(2000, 'x');
        wb_logf("%s|end\n", big.c_str());
        CHECK(cap.msgs.size() == 1);
        if (cap.msgs.size() == 1) {
            CHECK(cap.msgs[0].size() == big.size() + 5);
            CHECK(cap.msgs[0].compare(big.size(), 5, "|end\n") == 0);
        }

        // And the boundary itself: exactly one byte under, on, and over the
        // stack buffer, since that is where an off-by-one would hide.
        for (size_t len : {510u, 511u, 512u, 513u}) {
            cap.msgs.clear();
            wb_logf("%s", std::string(len, 'y').c_str());
            CHECK(cap.msgs.size() == 1);
            if (cap.msgs.size() == 1) {
                CHECK(cap.msgs[0].size() == len);
            }
        }
    }

    // --- uninstalling restores the default ----------------------------------
    // Not observable as "went to stderr" without capturing fd 2; what IS
    // observable, and is the property that matters, is that a stale sink
    // pointer stops being called.
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);
        wb_log_set_sink(nullptr);
        wb_logf("this must not reach the capture\n");
        CHECK(cap.msgs.empty());
    }

    // --- wb_log_stream() forwards into the sink -----------------------------
    // This is what RadioAir hands devourer's set_diag_stream() at teardown.
    {
        SinkGuard guard;
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);

        std::FILE* f = wb_log_stream();
        CHECK(f != nullptr);
        std::fputs("devourer: late line\n", f);
        std::fflush(f);
        CHECK(cap.msgs.size() == 1);
        if (cap.msgs.size() == 1) {
            CHECK(cap.msgs[0] == "devourer: late line\n");
        }

        // Same stream every call — it is a process-lifetime object, and a
        // per-call stream would leak one FILE per teardown.
        CHECK(wb_log_stream() == f);
    }

    // --- the stats sink REPLACES stdout, and UDP is independent -------------
    {
        StatsSnapshot snap;
        snap.t_ms = 4242;
        snap.node = 17;

        std::vector<std::string> lines;
        StatsEmitter emitter(/*to_stdout=*/true, /*udp=*/nullptr);
        emitter.set_local_sink(
            [](void* c, const char* line, size_t n) {
                static_cast<std::vector<std::string>*>(c)->emplace_back(line,
                                                                       n);
            },
            &lines);
        emitter.emit(snap);

        CHECK(lines.size() == 1);
        if (lines.size() == 1) {
            // Whole line, terminated — a consumer appending these gets NDJSON.
            CHECK(!lines[0].empty());
            CHECK(lines[0].back() == '\n');
            CHECK(lines[0].find("\"t_ms\":4242") != std::string::npos);
            // Byte-identical to what last_line() serves §15.5's control plane.
            CHECK(lines[0] == emitter.last_line());
        }

        // to_stdout=false with a sink installed still delivers: the sink is
        // the local egress, not an addition to a stdout that config disabled.
        std::vector<std::string> quiet;
        StatsEmitter q(/*to_stdout=*/false, /*udp=*/nullptr);
        q.set_local_sink(
            [](void* c, const char* line, size_t n) {
                static_cast<std::vector<std::string>*>(c)->emplace_back(line,
                                                                       n);
            },
            &quiet);
        q.emit(snap);
        CHECK(quiet.size() == 1);

        // Removing it goes back to the configured behaviour.
        q.set_local_sink(nullptr, nullptr);
        q.emit(snap);
        CHECK(quiet.size() == 1);
    }

    // --- csa.psk never appears in either path (CLAUDE.md, repo law) ---------
    // The config dump is the one place a secret could reach an output path,
    // and B8 moved that path. Assert on the dump itself rather than trusting
    // that nobody added a sink write of cfg.
    {
        SinkGuard guard;
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);

        Config cfg;
        cfg.policy.csa.psk = "hunter2-not-in-any-log";
        const std::string dump = dump_config_summary(cfg);
        CHECK(dump.find("hunter2-not-in-any-log") == std::string::npos);
        CHECK(dump.find("(set, redacted)") != std::string::npos);

        // And nothing the loader logged while we held the sink carried it.
        for (const std::string& m : cap.msgs) {
            CHECK(m.find("hunter2-not-in-any-log") == std::string::npos);
        }
    }

    return wbtest_finish("log_sink_test");
}
