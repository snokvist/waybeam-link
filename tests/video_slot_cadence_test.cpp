// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/video_slot_cadence.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    VideoSlotCadence cadence(500);

    // No live video means no selector-state TX opportunity, even when the
    // summary has been due since boot.
    CHECK(!cadence.due(false, 0));
    CHECK(!cadence.due(false, 5000));
    CHECK_EQ_U(cadence.next_due_ms(), 0);

    // The first live slot may carry it. A failed/aborted send remains pending.
    CHECK(cadence.due(true, 5000));
    CHECK(cadence.due(true, 5001));
    CHECK_EQ_U(cadence.next_due_ms(), 0);

    // AirBackend::inject returns a submission/target count, not bytes. Zero
    // keeps the item pending; the usual real-backend return value 1 consumes
    // it and advances the cadence.
    CHECK(!cadence.note_submitted(0, 5001));
    CHECK(cadence.due(true, 5002));
    CHECK(cadence.note_submitted(1, 5002));
    CHECK_EQ_U(cadence.next_due_ms(), 5502);
    CHECK(!cadence.due(true, 5501));
    CHECK(cadence.due(true, 5502));

    // If it becomes due while video is idle, it coalesces until the next live
    // slot and schedules the following period from the actual send.
    CHECK(!cadence.due(false, 9000));
    CHECK(cadence.due(true, 9100));
    CHECK(cadence.note_submitted(1, 9100));
    CHECK_EQ_U(cadence.next_due_ms(), 9600);

    return wbtest_finish("video_slot_cadence_test");
}
