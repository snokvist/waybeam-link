#!/bin/sh
# mon-up.sh <ifname> <channel> [htmode]   — bring a Realtek WiFi netdev into
# monitor mode for the waybeam-link kernel-monitor air backend (air.kind
# "kernel-monitor"). Mirrors the vehicle's S99wfb monitor setup. Run once per
# adapter BEFORE launching waybeam-link; the `ifname` in the config must match.
#
#   craft (vehicle wlan0):  mon-up.sh wlan0 161 HT20
#   ground desk (per iface): mon-up.sh wlx84fc1450bcde 161 HT20
#
# Requires: the kernel WiFi driver loaded (8812eu / rtl88x2eu / rtl88x2cu) and
# `iw`. channel 161 = 5805 MHz (the §4.1 gate channel).
set -e
IF="${1:?usage: mon-up.sh <ifname> <channel> [htmode]}"
CHAN="${2:?usage: mon-up.sh <ifname> <channel> [htmode]}"
HT="${3:-HT20}"

# Down → monitor → up → raise MTU (default 1500 caps injection → MCS0 failsafe;
# S99wfb uses 4052) → channel → let the driver's per-rate TXAGC curve own power.
ip link set "$IF" down
iw dev "$IF" set monitor otherbss 2>/dev/null || iw dev "$IF" set type monitor
ip link set "$IF" up
ip link set "$IF" mtu 4052 || true
iw dev "$IF" set channel "$CHAN" "$HT"
iw dev "$IF" set txpower auto 2>/dev/null || true

echo "mon-up: $IF -> $(iw dev "$IF" info 2>/dev/null | grep -E 'type|channel|txpower' | tr '\n' ' ')"
