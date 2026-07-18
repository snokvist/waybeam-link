// SPDX-License-Identifier: GPL-2.0-or-later
// Native bench endpoint: real GStreamer H.265 AUs <-> production frame-SHM.
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wblink/frame_shm.h"
#include "wblink/frame_shm_format.h"

using namespace wblink;

namespace {

bool h265_idr(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 5 < n; ++i) {
        size_t off = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            off = i + 3;
        } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 &&
                   p[i + 3] == 1) {
            off = i + 4;
        } else {
            continue;
        }
        if (off < n) {
            const uint8_t type = static_cast<uint8_t>((p[off] >> 1) & 0x3f);
            if (type >= 16 && type <= 21) {
                return true;
            }
        }
    }
    return false;
}

bool annexb(const uint8_t* p, size_t n) {
    return n >= 4 && p[0] == 0 && p[1] == 0 &&
           (p[2] == 1 || (p[2] == 0 && p[3] == 1));
}

std::unique_ptr<FrameShmRing> attach_retry(const std::string& name,
                                           unsigned timeout_ms) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        auto r = FrameShmRing::attach(name);
        if (r) {
            return std::move(*r.value);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return nullptr;
}

int produce(const std::string& ring_name, uint32_t bitrate_kbps,
            uint32_t frames) {
    auto rr = FrameShmRing::create(ring_name);
    if (!rr) {
        std::fprintf(stderr, "%s\n", rr.error.c_str());
        return 1;
    }
    auto ring = std::move(*rr.value);
    const std::string desc =
        "videotestsrc is-live=true num-buffers=" + std::to_string(frames) +
        " pattern=snow ! video/x-raw,width=1280,height=720,framerate=30/1 "
        "! x265enc bitrate=" + std::to_string(bitrate_kbps) +
        " key-int-max=30 tune=zerolatency speed-preset=ultrafast "
        "! h265parse config-interval=-1 "
        "! video/x-h265,stream-format=byte-stream,alignment=au "
        "! appsink name=sink sync=false max-buffers=8 drop=false";
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.c_str(), &err);
    if (pipeline == nullptr) {
        std::fprintf(stderr, "gst producer: %s\n", err ? err->message : "parse failed");
        if (err) g_error_free(err);
        return 1;
    }
    GstAppSink* sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "sink"));
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    uint64_t written = 0;
    uint64_t bytes = 0;
    uint64_t idrs = 0;
    while (written < frames) {
        GstSample* sample = gst_app_sink_try_pull_sample(sink, 2 * GST_SECOND);
        if (sample == nullptr) break;
        GstBuffer* b = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        if (gst_buffer_map(b, &map, GST_MAP_READ)) {
            std::vector<uint8_t> blob(kVencFrameMetaSize + map.size);
            VencFrameMeta meta{};
            const GstClockTime pts = GST_BUFFER_PTS(b);
            meta.pts = GST_CLOCK_TIME_IS_VALID(pts)
                           ? static_cast<uint32_t>(pts / GST_MSECOND)
                           : static_cast<uint32_t>(written * 1000 / 30);
            meta.codec = kFrameCodecH265;
            meta.flags = h265_idr(map.data, map.size) ? kFrameFlagIdr : 0;
            std::memcpy(blob.data(), &meta, sizeof(meta));
            std::memcpy(blob.data() + kVencFrameMetaSize, map.data, map.size);
            if (ring->write_frame(blob.data(), blob.size())) {
                ++written;
                bytes += map.size;
                idrs += meta.flags != 0 ? 1u : 0u;
            }
            gst_buffer_unmap(b, &map);
        }
        gst_sample_unref(sample);
    }
    gst_element_send_event(pipeline, gst_event_new_eos());
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    const auto& s = ring->stats();
    std::printf("producer frames=%llu idr=%llu bytes=%llu full_drop=%llu "
                "oversize_drop=%llu\n",
                static_cast<unsigned long long>(written),
                static_cast<unsigned long long>(idrs),
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(s.full_drops),
                static_cast<unsigned long long>(s.oversize_drops));
    return written == frames && idrs > 0 && s.full_drops == 0 &&
                   s.oversize_drops == 0
               ? 0
               : 2;
}

int consume(const std::string& ring_name, uint32_t expected_frames,
            uint32_t timeout_ms, const char* trace_path = nullptr) {
    auto ring = attach_retry(ring_name, timeout_ms);
    if (!ring) {
        std::fprintf(stderr, "consumer: ring %s did not appear\n", ring_name.c_str());
        return 1;
    }
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "appsrc name=src is-live=true format=time "
        "caps=video/x-h265,stream-format=byte-stream,alignment=au "
        "! h265parse ! avdec_h265 ! fakesink sync=false", &err);
    if (pipeline == nullptr) {
        std::fprintf(stderr, "gst consumer: %s\n", err ? err->message : "parse failed");
        if (err) g_error_free(err);
        return 1;
    }
    GstAppSrc* src = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "src"));
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::vector<uint8_t> blob(kFrameRingDefaultSlotSize);
    uint64_t frames = 0;
    uint64_t idrs = 0;
    uint64_t bad_meta = 0;
    uint64_t bad_annexb = 0;
    uint64_t pts_regress = 0;
    uint32_t last_pts = 0;
    std::FILE* trace = nullptr;
    if (trace_path != nullptr) {
        trace = std::fopen(trace_path, "w");
        if (!trace) {
            std::perror("consumer trace");
            return 1;
        }
        std::fprintf(trace, "frame,arrival_ns,pts,bytes,idr\n");
    }
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (frames < expected_frames && std::chrono::steady_clock::now() < end) {
        const long n = ring->read_frame(blob.data(), blob.size());
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (static_cast<size_t>(n) < kVencFrameMetaSize) {
            ++bad_meta;
            continue;
        }
        VencFrameMeta meta{};
        std::memcpy(&meta, blob.data(), sizeof(meta));
        const uint8_t* data = blob.data() + kVencFrameMetaSize;
        const size_t len = static_cast<size_t>(n) - kVencFrameMetaSize;
        const bool gdr = (meta.flags & kFrameFlagGdr) != 0;
        const bool bad_gdr = gdr ? meta.gdr_len == 0 || meta.gdr_pos >= meta.gdr_len
                                 : meta.gdr_pos != 0 || meta.gdr_len != 0;
        bad_meta += meta.codec != kFrameCodecH265 ||
                            (meta.flags & ~kFrameFlagsKnown) != 0 || bad_gdr
                        ? 1u
                        : 0u;
        bad_annexb += annexb(data, len) ? 0u : 1u;
        pts_regress += frames > 0 && meta.pts < last_pts &&
                               last_pts - meta.pts < 0x80000000u
                           ? 1u
                           : 0u;
        last_pts = meta.pts;
        idrs += (meta.flags & kFrameFlagIdr) != 0 ? 1u : 0u;
        if (trace) {
            const auto arrival_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
            std::fprintf(trace, "%llu,%lld,%u,%zu,%u\n",
                         static_cast<unsigned long long>(frames),
                         static_cast<long long>(arrival_ns), meta.pts, len,
                         (meta.flags & kFrameFlagIdr) != 0 ? 1u : 0u);
        }
        GstBuffer* b = gst_buffer_new_allocate(nullptr, len, nullptr);
        gst_buffer_fill(b, 0, data, len);
        GST_BUFFER_PTS(b) = static_cast<GstClockTime>(meta.pts) * GST_MSECOND;
        if (gst_app_src_push_buffer(src, b) != GST_FLOW_OK) {
            ++bad_meta;
            break;
        }
        ++frames;
    }
    gst_app_src_end_of_stream(src);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    bool decode_ok = msg != nullptr && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* gst_err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &gst_err, &debug);
        std::fprintf(stderr, "decoder: %s\n", gst_err ? gst_err->message : "error");
        if (gst_err) g_error_free(gst_err);
        g_free(debug);
    }
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(pipeline);
    if (trace) std::fclose(trace);
    std::printf("consumer frames=%llu idr=%llu bad_meta=%llu bad_annexb=%llu "
                "pts_regress=%llu decode=%s\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(idrs),
                static_cast<unsigned long long>(bad_meta),
                static_cast<unsigned long long>(bad_annexb),
                static_cast<unsigned long long>(pts_regress),
                decode_ok ? "ok" : "fail");
    return frames == expected_frames && bad_meta == 0 && bad_annexb == 0 &&
                   pts_regress == 0 && decode_ok
               ? 0
               : 2;
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s produce RING BITRATE_KBPS FRAMES\n"
                     "       %s consume RING FRAMES TIMEOUT_MS\n"
                     "       %s consume-trace RING FRAMES TIMEOUT_MS TRACE.csv\n",
                     argv[0], argv[0], argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    const std::string ring = argv[2];
    const uint32_t a = static_cast<uint32_t>(std::stoul(argv[3]));
    const uint32_t b = static_cast<uint32_t>(std::stoul(argv[4]));
    if (mode == "produce") return produce(ring, a, b);
    if (mode == "consume") return consume(ring, a, b);
    if (mode == "consume-trace" && argc == 6) {
        return consume(ring, a, b, argv[5]);
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 1;
}
