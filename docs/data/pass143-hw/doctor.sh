#!/usr/bin/env bash
# H1 triage — devourer's adapter-health probes against the bench pair.
#
# The hypothesis worth testing: AdapterHealth.h's motivating failure is a chip
# that enumerates fine and inits green while its EFUSE reads return STOCHASTIC
# content, so the driver loads the wrong RFE/PA/LNA tables. That is what a unit
# which transmits but cannot sustain 64-QAM would look like.
#
#   1-1  8822EU — the H1 suspect (fails MCS5-7 under devourer, 99.96% under
#                 the kernel driver at MCS7)
#   5-1  8812CU — control, no known TX problem
#
# A healthy chip returns byte-identical EFUSE content on every physical read.
set -u
DOC=/tmp/claude-1000/-home-snokvist-dev-waybeam-coordination/7de0f182-9459-46f7-a097-a53febbb617a/scratchpad/dv/doctor

sudo systemctl stop waybeam-ground 2>/dev/null
sleep 1
for d in rtl88x2cu:5-1:1.0 rtl88x2eu:1-1:1.0; do
    echo "${d#*:}" | sudo tee "/sys/bus/usb/drivers/${d%%:*}/unbind" >/dev/null 2>&1
done
sleep 2
ls /sys/class/net | grep -q wlx && echo "WARN: a netdev is still bound"

for probe in "1 1 8822EU-suspect" "5 1 8812CU-control"; do
    set -- $probe
    echo "=================== $3  (bus $1 port $2) ==================="
    sudo "$DOC" --bus "$1" --port "$2" --reads 8 --channel 165 --listen-secs 4 2>&1 \
        | grep -vE "devourer \[[ID]\]" | tail -40
done
