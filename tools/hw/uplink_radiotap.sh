#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# P0c — summarize the GROUND's own uplink radiotap parameters.
#
# Pass 125 made an rx-node call set_tx_mode(air.uplink_rate) for the first
# time; the claim is that the seeds {mcs:0, sgi:false, bw:20} are byte-identical
# to the TxRate struct default an rx-node used to leave untouched. That is a
# claim about UNCHANGED behaviour, so it has to be measured before and after,
# not asserted.
#
# The ground's frames carry SA 56:42:00:00:<originator>:<adapter-index>, so
# originator 9 is 56:42:00:00:09:xx. Everything else on the air is the craft.
IF=${IF:-wlx84fc1450bcde}
ORIG_HEX=${ORIG_HEX:-09}
N=${N:-400}
SECS=${SECS:-20}

echo "capturing $N frames from originator 0x$ORIG_HEX on $IF (max ${SECS}s)"
sudo -n timeout "$SECS" tcpdump -i "$IF" -c "$N" -e -nn \
    "ether[10:4] = 0x56420000 and ether[14:2] = 0x${ORIG_HEX}00" 2>/dev/null \
| awk '
{
    mcs="?"; gi="LGI"; bw="?"
    for (i=1;i<=NF;i++) {
        if ($i=="MCS")        mcs=$(i+1)
        if ($i=="short"&&$(i+1)=="GI") gi="SGI"
        if ($i=="MHz"&&$(i-1)~/^[0-9]+$/&&$(i-1)<200) bw=$(i-1)
    }
    key=mcs" "gi" "bw"MHz"; c[key]++; n++
}
END {
    if (n==0) { print "  NO FRAMES from this originator"; exit 1 }
    printf "  %d frames\n", n
    for (k in c) printf "  %6d  %5.1f%%  MCS %s\n", c[k], 100*c[k]/n, k
}'
