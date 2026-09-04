#pragma once

/* AckResponder — the hardware ACK engine as a first-class monitor-mode knob.
 *
 * The Realtek MAC auto-ACKs (SIFS-timed, zero host involvement) any unicast
 * frame whose RA matches the port-0 MACID, PROVIDED the port has a network
 * type. devourer's monitor bring-up leaves net_type = 0 (No Link), which is
 * why a monitor radio never ACKs; the AP-mode work proved the gate — with
 * MACID + net_type programmed, a real station's auth/assoc arrive at retry=0
 * (docs/ap-mode.md). This header is that recipe minus the beacon machinery:
 * program the port identity, flip the net type, and the chip becomes an ACK
 * responder for one MAC address while everything else about monitor mode
 * (promiscuous RX, injection) is unchanged.
 *
 * On the adapter combinations exercised by tests/ampdu_ba_check.sh, the SAME
 * gate also enables the hardware BlockAck responder. RTL8733B has its own
 * air-side proof in tests/rtl8733b_blockack_onair.sh: a Jaguar2 TX forms real
 * A-MPDUs and an independent Jaguar1 witness observes retry copies collapse
 * only while the RTL8733B responder is armed.
 *
 * The registers are generation-neutral (same map on Jaguar1/2/3 and
 * RTL8733B):
 *   0x0610..0x0615  REG_MACID   — the RA the ACK engine matches
 *   0x0618..0x061d  REG_BSSID   — port identity companion (the proven AP
 *                                 recipe programs both)
 *   0x0102[1:0]     MSR/net_type (REG_CR+2) — 0 NoLink / 1 Ad-hoc / 2 Infra /
 *                                 3 AP; any nonzero arms the responder. We
 *                                 use AP (3), the bench-proven value.
 *
 * Turning a passive monitor into an ACTIVE transmitter is a behavioral
 * change — hence opt-in only (DeviceConfig rx.ack_responder / the
 * SetAckResponder runtime call), never a default. The MAC must be UNICAST
 * (I/G bit clear) — a station cannot ACK-target a group address, and the
 * same footgun broke AP association (docs/ap-mode.md). */

#include <cstdint>

#include "RtlAdapter.h"

namespace devourer {
namespace ack {

inline bool enable(RtlAdapter &dev, const uint8_t mac[6]) noexcept {
  try {
    const uint8_t nt = dev.rtw_read8(0x0102);
    /* Close the gate before changing identity. Besides avoiding a transient
     * responder for a half-written MAC during retargeting, this makes every
     * failed identity write leave the radio passive. */
    if (!dev.rtw_write8(0x0102, static_cast<uint8_t>(nt & ~0x03u))) {
      /* The transfer status is not state readback. Retry the safety clear using
       * the value read before the failed transfer; callers verify it before
       * reporting a failed arm as passive. */
      (void)dev.rtw_write8(0x0102, static_cast<uint8_t>(nt & ~0x03u));
      return false;
    }
    if (!dev.rtw_write<uint32_t>(
            0x0610, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                        ((uint32_t)mac[2] << 16) |
                        ((uint32_t)mac[3] << 24)) ||
        !dev.rtw_write16(0x0614, (uint16_t)(mac[4] | (mac[5] << 8))) ||
        !dev.rtw_write<uint32_t>(
            0x0618, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                        ((uint32_t)mac[2] << 16) |
                        ((uint32_t)mac[3] << 24)) ||
        !dev.rtw_write16(0x061c, (uint16_t)(mac[4] | (mac[5] << 8))))
      return false;
    if (dev.rtw_write8(0x0102,
                       static_cast<uint8_t>((nt & ~0x03u) | 0x03u)))
      return true;
    /* A failed status does not prove the gate write had no side effect. Make a
     * best-effort close before reporting failure; callers additionally verify
     * the passive rollback. */
    (void)dev.rtw_write8(0x0102, static_cast<uint8_t>(nt & ~0x03u));
    return false;
  } catch (...) {
    /* SetAckResponder is a bool contract. Its callers perform a verified
     * rollback and report UNKNOWN state if transport reads remain unavailable. */
    return false;
  }
}

/* Disarm: net_type back to No Link — the gate, so the MACID may stay. */
inline bool disable(RtlAdapter &dev) {
  const uint8_t nt = dev.rtw_read8(0x0102);
  return dev.rtw_write8(0x0102, static_cast<uint8_t>(nt & ~0x03u));
}

inline bool is_disabled(RtlAdapter &dev) {
  return (dev.rtw_read8(0x0102) & 0x03u) == 0;
}

inline bool disable_verified(RtlAdapter &dev) noexcept {
  try {
    (void)disable(dev);
    /* Readback is the safety result: a control transfer may report failure even
     * though the write landed, while a successful transfer alone proves no
     * state. The caller only needs to know whether the active gate is closed. */
    return is_disabled(dev);
  } catch (...) {
    return false;
  }
}

/* The MAC must be UNICAST: a station cannot ACK-target a group address, so an
 * arm on one can never fire. Lives here rather than in each backend because
 * the precondition is a property of the recipe, not of any one die. */
inline bool is_unicast(const uint8_t mac[6]) { return (mac[0] & 0x01u) == 0; }

/* Did the arm actually land? Reads back the gate (net_type) and the RA the ACK
 * engine matches (MACID), composed exactly as enable() writes them — keeping
 * the register map in ONE file, so a change to enable() cannot silently
 * diverge from a copy of it somewhere else.
 *
 * NB the BSSID companion at 0x0618 is written but not checked: the ACK engine
 * matches on MACID, and 0x0618 is programmed only because the proven AP recipe
 * programs both. Verifying the two fields that gate the behaviour keeps this
 * honest without asserting on one that does not. */
inline bool verify(RtlAdapter &dev, const uint8_t mac[6]) noexcept {
  try {
    const uint32_t want_lo =
        (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
        ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    const uint16_t want_hi = (uint16_t)(mac[4] | (mac[5] << 8));
    return (dev.rtw_read8(0x0102) & 0x03u) == 0x03u &&
           dev.rtw_read<uint32_t>(0x0610) == want_lo &&
           dev.rtw_read16(0x0614) == want_hi;
  } catch (...) {
    return false;
  }
}

} /* namespace ack */
} /* namespace devourer */
