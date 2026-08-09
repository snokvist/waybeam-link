// SPDX-License-Identifier: GPL-2.0-or-later
// §7.2 frame-kind predicates (#109 Phase 2c).
//
// The three move together although only the first two are read by the RX
// loop: they are one family that decodes a frame and asks a single question
// of the §3.1 header, and splitting the family across the layer boundary
// would leave the odd one out looking like a driver concern, which it is not.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>

#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {
namespace node {

// §7.2: the pacer keys off END_OF_BLOCK frames in both directions.
inline bool frame_is_eob(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr && (v->hdr.data_flags & data_flags::kEndOfBlock) != 0;
}

// §7.2 Pass 78 paced-stream semantics: only the RTP video stream's EOBs open
// craft listen windows / re-anchor ground returns. A non-video datagram is a
// one-datagram block whose EOB must not re-arm the gap (50 Hz audio EOBs
// re-arming mid-flush is the measured rung-flapping failure).
inline bool frame_is_paced_eob(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr &&
           (v->hdr.data_flags & data_flags::kEndOfBlock) != 0 &&
           v->hdr.stream_type == stream_type::kRtp;
}

inline bool frame_is_live_rtp_data(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr && v->hdr.stream_type == stream_type::kRtp &&
           (v->hdr.data_flags & data_flags::kRetransmit) == 0;
}

}  // namespace node
}  // namespace wblink
