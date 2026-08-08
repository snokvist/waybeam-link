// SPDX-License-Identifier: GPL-2.0-or-later
// B8 (issue #144): the two injected sinks. Diagnostics used to go to a
// hardcoded stderr and the §15.3 stats line to a hardcoded stdout, so a
// library consumer got noise it could not read and lost telemetry it needed.
//
// The properties worth asserting are not "the callback fires". They are the
// ones a refactor could quietly break and a consumer would discover in the
// field: the DEFAULTS still write to the same file descriptors they always
// did (this is what protects every flying node), the stats sink REPLACES
// stdout rather than adding to it, the UDP binding is independent of both,
// long messages are not truncated, and csa.psk reaches neither path.
//
// Several of those are only observable by capturing fd 1 and fd 2, so this
// test does that rather than asserting the weaker "our callback got called".
#include "wblink/log.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "wblink/binding.h"
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
// cannot leave a later block logging through a sink whose cookie has gone.
// DECLARE IT LAST: destruction is reverse-declaration order, so the guard has
// to be the first thing destroyed, i.e. the last thing declared.
struct SinkGuard {
    ~SinkGuard() { wb_log_set_sink(nullptr); }
};

// Redirects one fd to a temp file for the lifetime of the object and hands
// back what was written. This is the only way to check where the DEFAULT
// sinks go — the whole point being that they still go where they used to.
class FdCapture {
  public:
    explicit FdCapture(int fd) : fd_(fd) {
        saved_ = ::dup(fd_);
        char path[] = "/tmp/wblink_log_sink_test_XXXXXX";
        tmp_ = ::mkstemp(path);
        ::unlink(path);
        if (saved_ >= 0 && tmp_ >= 0) {
            ::dup2(tmp_, fd_);
        }
    }

    // Flush the C library's own buffer for this fd before reading it back,
    // or a line still sitting in stdout's buffer reads as "nothing written".
    std::string text() {
        std::fflush(fd_ == 1 ? stdout : stderr);
        std::string out;
        if (tmp_ < 0) {
            return out;
        }
        ::lseek(tmp_, 0, SEEK_SET);
        char buf[8192];
        ssize_t n;
        while ((n = ::read(tmp_, buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    ~FdCapture() {
        std::fflush(fd_ == 1 ? stdout : stderr);
        if (saved_ >= 0) {
            ::dup2(saved_, fd_);
            ::close(saved_);
        }
        if (tmp_ >= 0) {
            ::close(tmp_);
        }
    }

  private:
    int fd_;
    int saved_ = -1;
    int tmp_ = -1;
};

}  // namespace

int main() {
    // --- diagnostics reach an installed sink, whole and unterminated --------
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        SinkGuard guard;
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

    // --- THE DEFAULT still goes to fd 2, and nothing to fd 1 ----------------
    // This is the check that protects every flying node from this commit: the
    // daemon's diagnostics must land on stderr exactly as before. It matters
    // in the other direction too — a diagnostic leaking to stdout corrupts
    // the NDJSON stats stream a consumer is parsing, and it would do so only
    // when something is already going wrong.
    {
        wb_log_set_sink(nullptr);
        FdCapture err(2);
        FdCapture out(1);
        wb_logf("radio: default sink probe %d\n", 7);
        wb_log_write("pre-formatted probe\n", 20);
        const std::string e = err.text();
        const std::string o = out.text();
        CHECK(e == "radio: default sink probe 7\npre-formatted probe\n");
        CHECK(o.empty());
    }

    // --- the long-message branch does not truncate --------------------------
    // wb_logf formats into a 512-byte stack buffer and only then allocates.
    // A silently truncated diagnostic loses its tail, which is where the bus
    // path and the MAC live — the two things that make it worth logging.
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        SinkGuard guard;
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

    // --- uninstalling detaches the stale pointer -----------------------------
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        wb_log_set_sink(&sink);
        wb_log_set_sink(nullptr);
        FdCapture err(2);  // the default takes it back; keep it off the console
        wb_logf("this must not reach the capture\n");
        CHECK(!err.text().empty());
        CHECK(cap.msgs.empty());
    }

    // --- wb_log_stream() forwards into the sink, UNFLUSHED -------------------
    // This is what RadioAir hands devourer's set_diag_stream() at teardown.
    // Deliberately no fflush: the stream is unbuffered because a buffered one
    // both splits messages mid-line and leaves residue for the C library's
    // exit-time cleanup to flush — which runs after a consumer's sink object
    // has been destroyed. An fflush here would hide exactly that.
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        SinkGuard guard;
        wb_log_set_sink(&sink);

        std::FILE* f = wb_log_stream();
        CHECK(f != nullptr);
        std::fputs("devourer: late line\n", f);
        CHECK(cap.msgs.size() == 1);
        if (cap.msgs.size() == 1) {
            CHECK(cap.msgs[0] == "devourer: late line\n");
        }

        // Enough to overflow a default 8192-byte buffer, delivered as whole
        // messages rather than one mid-line chunk.
        cap.msgs.clear();
        for (int i = 0; i < 300; ++i) {
            std::fputs("devourer: 0123456789012345678901234567890123456789\n",
                       f);
        }
        CHECK(cap.msgs.size() == 300);
        bool all_whole = true;
        for (const std::string& m : cap.msgs) {
            if (m.empty() || m.back() != '\n') all_whole = false;
        }
        CHECK(all_whole);

        // Same stream every call — it is a process-lifetime object, and a
        // per-call stream would leak one FILE per teardown.
        CHECK(wb_log_stream() == f);
    }

    // --- the stats sink REPLACES stdout -------------------------------------
    {
        StatsSnapshot snap;
        snap.t_ms = 4242;
        snap.node = 17;

        std::vector<std::string> lines;
        std::string on_stdout;
        {
            FdCapture out(1);
            StatsEmitter emitter(/*to_stdout=*/true, /*udp=*/nullptr);
            emitter.set_local_sink(
                [](void* c, const char* line, size_t n) {
                    static_cast<std::vector<std::string>*>(c)->emplace_back(
                        line, n);
                },
                &lines);
            emitter.emit(snap);
            on_stdout = out.text();

            CHECK(lines.size() == 1);
            if (lines.size() == 1) {
                // Whole line, terminated — appending these yields NDJSON.
                CHECK(lines[0].back() == '\n');
                CHECK(lines[0].find("\"t_ms\":4242") != std::string::npos);
                // Byte-identical to what last_line() serves §15.5.
                CHECK(lines[0] == emitter.last_line());
            }
        }
        // REPLACES, not adds: to_stdout was true and stdout still got nothing.
        // Without this the "double-emit" regression is invisible.
        CHECK(on_stdout.empty());
    }

    // --- with no sink, the default still writes the line to stdout ----------
    {
        StatsSnapshot snap;
        snap.t_ms = 909;
        std::string on_stdout;
        std::string expect;
        {
            FdCapture out(1);
            StatsEmitter emitter(/*to_stdout=*/true, /*udp=*/nullptr);
            emitter.emit(snap);
            on_stdout = out.text();
            expect = emitter.last_line();
        }
        CHECK(on_stdout == expect);
        CHECK(!expect.empty());

        // And stats.stdout=false with no sink still writes nothing.
        std::string quiet_out;
        {
            FdCapture out(1);
            StatsEmitter q(/*to_stdout=*/false, /*udp=*/nullptr);
            q.emit(snap);
            quiet_out = out.text();
        }
        CHECK(quiet_out.empty());
    }

    // --- the UDP binding is independent of the local sink -------------------
    {
        auto in = UdpIngress::open("127.0.0.1:0");
        CHECK(bool(in));
        auto eg = UdpEgress::open("127.0.0.1:" +
                                  std::to_string(in.value->bound_port()));
        CHECK(bool(eg));

        StatsSnapshot snap;
        snap.t_ms = 5150;
        std::vector<std::string> lines;
        StatsEmitter emitter(/*to_stdout=*/true, &*eg.value);
        emitter.set_local_sink(
            [](void* c, const char* line, size_t n) {
                static_cast<std::vector<std::string>*>(c)->emplace_back(line,
                                                                       n);
            },
            &lines);
        {
            FdCapture out(1);
            emitter.emit(snap);
            CHECK(out.text().empty());
        }

        uint8_t buf[16384];
        long n = 0;
        for (int tries = 0; tries < 100 && n <= 0; ++tries) {
            n = in.value->recv_one(buf, sizeof(buf));
        }
        CHECK(n > 0);
        CHECK(lines.size() == 1);
        // Installing a local sink neither suppresses nor duplicates the UDP
        // egress, and both carry the identical bytes.
        if (n > 0 && lines.size() == 1) {
            CHECK(static_cast<size_t>(n) == lines[0].size());
            CHECK(std::memcmp(buf, lines[0].data(), lines[0].size()) == 0);
        }
    }

    // --- csa.psk reaches neither path (CLAUDE.md, repo law) -----------------
    // Load a config that actually carries a psk, rather than default-
    // constructing one — the loader is the code that touches the secret, and
    // a check that never runs it proves nothing.
    {
        Capture cap;
        const LogSink sink{&Capture::write, &cap};
        SinkGuard guard;
        wb_log_set_sink(&sink);

        const char* kSecret = "hunter2-must-not-appear";
        std::string json = R"({
          "node": {"originator": 17, "role": "tx", "net_id": 0},
          "profile_table": "profiles/table.example.json",
          "adapters": [{"name":"a0","role":"tx","channel":5805,"bw":20,
                        "max_power_qdb": 40,
                        "power_presets_qdb": [20, 60]}],
          "streams": [],
          "policy": {"csa": {"psk": ")" +
                           std::string(kSecret) + R"("}}
        })";
        auto loaded = load_config_json(json);
        CHECK(bool(loaded));
        if (loaded) {
            CHECK(loaded.value->policy.csa.psk == kSecret);
            const std::string dump = dump_config_summary(*loaded.value);
            CHECK(dump.find(kSecret) == std::string::npos);
            CHECK(dump.find("(set, redacted)") != std::string::npos);
        }

        // The loader DID log while we held the sink — the §10.3 power clamp
        // fires on the preset above — so this loop is not vacuous.
        CHECK(!cap.msgs.empty());
        for (const std::string& m : cap.msgs) {
            CHECK(m.find(kSecret) == std::string::npos);
        }
    }

    return wbtest_finish("log_sink_test");
}
