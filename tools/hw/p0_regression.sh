#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# P0 — regression on ALREADY-SHIPPED behaviour, before any new-feature work.
#
# Pass 125/126 changed three things on code that was already deployed, and
# every one of them is invisible to the §10.7 tests:
#   P0b  config-load rejection re-keying (power_map by ADAPTER role) and the
#        new seek_step_qdb >= 8 gate — a live config that used to load must
#        still load.
#   P0a  §10.6 craft calibration. The Pass 125 floor rule changed the bottom of
#        every rung-0 ramp, and W9 changed what the addendum-4 blackout retreat
#        records as the rung's overload bracket. Compare against the last
#        known-good artifact.
#   P0c  radiotap bytes on the ground uplink — an rx-node now calls
#        set_tx_mode(air.uplink_rate). The claim is that the seeds {0,false,20}
#        are byte-identical to the previous TxRate default; a claim about
#        UNCHANGED behaviour has to be measured, not asserted.
set -u
. "$(dirname "$0")/lib.sh"
HW_PASS=0; HW_FAIL=0
STAGE=/tmp/wblink-new

log "P0b: new binary --check against the LIVE unmodified configs"

log "  staging craft binary"
craft "cat > $STAGE && chmod 755 $STAGE" < "$(dirname "$0")/../../build/ssc338q/waybeam-link" || exit 1
out=$(craft "cd /etc/waybeam-link && $STAGE tx -c /etc/waybeam-link/craft.json --check 2>&1"; echo "rc=$?")
echo "$out" | sed 's/^/    craft: /'
case "$out" in *"rc=0"*) pass "craft config still loads" ;; *) fail "craft config REJECTED by new binary" ;; esac

log "  staging ground binary"
ground "cat > $STAGE && chmod 755 $STAGE" < "$(dirname "$0")/../../build/rk3566/waybeam-link" || exit 1
out=$(ground "cd /etc/waybeam-link && $STAGE rx -c /etc/waybeam-link/ground.json --check 2>&1"; echo "rc=$?")
echo "$out" | sed 's/^/    ground: /'
case "$out" in *"rc=0"*) pass "ground config still loads" ;; *) fail "ground config REJECTED by new binary" ;; esac

hw_summary "P0b"
