// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/recovery.h"

#include <algorithm>

namespace wblink {

std::vector<StreamKey> LatchRecovery::due(
    const std::vector<LatchStream>& latched, uint64_t now_ms) {
    std::vector<StreamKey> out;
    // Forget keys that are no longer latched, so a re-latch re-arms (§3.9).
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const Entry& e) {
                           return std::none_of(
                               latched.begin(), latched.end(),
                               [&](const LatchStream& s) {
                                   return s.key == e.key;
                               });
                       }),
        entries_.end());

    for (const LatchStream& s : latched) {
        Entry* e = nullptr;
        for (Entry& cand : entries_) {
            if (cand.key == s.key) {
                e = &cand;
                break;
            }
        }
        if (e == nullptr) {
            // First sight of this key: due immediately, schedule from now.
            entries_.push_back(Entry{s.key, s.local_stream_id, 0, now_ms,
                                     false});
            e = &entries_.back();
        }
        // The local id can change if the same wire stream is re-bound; keep the
        // observation target current so note_irap() still reaches this entry.
        e->local_stream_id = s.local_stream_id;
        if (e->done || e->attempts >= kAttempts || now_ms < e->next_ms) {
            continue;
        }
        ++e->attempts;
        e->next_ms = now_ms + kPeriodMs;
        out.push_back(e->key);
    }
    return out;
}

void LatchRecovery::note_irap(uint8_t local_stream_id) {
    for (Entry& e : entries_) {
        if (e.local_stream_id == local_stream_id) {
            e.done = true;
        }
    }
}

const LatchRecovery::Entry* LatchRecovery::find(const StreamKey& key) const {
    for (const Entry& e : entries_) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

uint8_t LatchRecovery::attempts(const StreamKey& key) const {
    const Entry* e = find(key);
    return e != nullptr ? e->attempts : 0;
}

bool LatchRecovery::settled(const StreamKey& key) const {
    const Entry* e = find(key);
    if (e == nullptr) return false;
    return e->done || e->attempts >= kAttempts;
}

}  // namespace wblink
