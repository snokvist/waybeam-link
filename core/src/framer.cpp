// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/framer.h"

#include "wblink/rtp.h"

namespace wblink {

bool Framer::on_datagram(const uint8_t* data, size_t len, uint64_t now_ms,
                         const Emit& emit) {
    ++stats_.datagrams;
    if (len > kMaxDataPayload) {
        ++stats_.oversize_ingress;
        return false;
    }

    bool end_of_block = false;

    if (cfg_.stream_type == stream_type::kRtp) {
        const auto rtp = parse_rtp_header(data, len);
        // Boundary: previous datagram carried the marker, or the timestamp
        // moved without one. Either way this datagram opens a new block.
        bool new_block = !block_open_;
        if (prev_was_marker_) {
            new_block = true;
        } else if (rtp && have_rtp_ts_ && rtp->timestamp != last_rtp_ts_) {
            new_block = true;
        }
        if (new_block && block_open_) {
            ++block_id_;
        }
        if (new_block) {
            block_open_ = true;
            block_bytes_ = 0;
            block_arq_ = false;
            ++stats_.blocks;
        }
        if (rtp) {
            have_rtp_ts_ = true;
            last_rtp_ts_ = rtp->timestamp;
            prev_was_marker_ = rtp->marker;
            end_of_block = rtp->marker;
            // §4.1 NAL classifier: stamps from the first packet of the block
            // (FU fragments repeat the type), sticky for the block's rest.
            if (cfg_.classifier != RtpClassifier::kSize && !block_arq_) {
                if (const auto pl = rtp_payload(data, len, *rtp)) {
                    block_arq_ = cfg_.classifier == RtpClassifier::kH264
                                     ? h264_payload_important(pl->data, pl->len)
                                     : h265_payload_important(pl->data, pl->len);
                }
            }
        } else {
            // Unparseable on an RTP stream: close the block defensively so a
            // junk datagram cannot glue two real frames together.
            prev_was_marker_ = true;
            end_of_block = true;
        }
    } else {
        // Non-RTP profiles: one datagram = one block (§4).
        if (block_open_) {
            ++block_id_;
        }
        block_open_ = true;
        block_bytes_ = 0;
        block_arq_ = false;
        ++stats_.blocks;
        end_of_block = true;
    }

    block_bytes_ += static_cast<uint32_t>(len);
    if (cfg_.stream_type == stream_type::kRtp &&
        cfg_.classifier == RtpClassifier::kSize &&
        block_bytes_ >= cfg_.classifier_size_threshold) {
        block_arq_ = true;
    }

    DataHeader hdr;
    hdr.prefix.originator = cfg_.originator;
    hdr.prefix.destination = cfg_.destination;
    hdr.prefix.session_id = cfg_.session_id;
    hdr.stream_id = cfg_.stream_id;
    hdr.stream_type = cfg_.stream_type;
    hdr.seq = next_seq_++;
    hdr.block_id = block_id_;
    hdr.data_flags = static_cast<uint8_t>(
        (end_of_block ? data_flags::kEndOfBlock : 0) |
        (block_arq_ ? data_flags::kArq : 0) | extra_flags_);
    hdr.active_profile = active_profile_;
    hdr.table_version = table_version_;

    uint8_t frame[kDataHeaderSize + kMaxDataPayload];
    const size_t n = encode_data(hdr, data, static_cast<uint16_t>(len), frame,
                                 sizeof(frame));
    if (n == 0) {
        return false;  // unreachable given the length check above
    }
    ++stats_.frames;
    emit(frame, n, hdr, now_ms);
    return true;
}

}  // namespace wblink
