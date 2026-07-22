#!/bin/sh
# Wait for all named network interfaces to appear.  A bounded failure lets the
# supervising service retry forever without holding boot completion hostage.
set -eu

TIMEOUT="${WAYBEAM_IFACE_WAIT_SECONDS:-30}"
case "$TIMEOUT" in
	''|*[!0-9]*)
		echo "waybeam-wait-ifaces: invalid timeout: $TIMEOUT" >&2
		exit 2
		;;
esac

if [ "$#" -eq 0 ]; then
	echo "usage: waybeam-wait-ifaces <ifname>..." >&2
	exit 2
fi

waited=0
while :; do
	missing=""
	for iface in "$@"; do
		if [ ! -e "/sys/class/net/$iface" ]; then
			missing="$missing $iface"
		fi
	done

	if [ -z "$missing" ]; then
		echo "waybeam-wait-ifaces: ready:$*"
		exit 0
	fi
	if [ "$waited" -ge "$TIMEOUT" ]; then
		echo "waybeam-wait-ifaces: timed out waiting for:$missing" >&2
		exit 1
	fi
	if [ "$waited" -eq 0 ]; then
		echo "waybeam-wait-ifaces: waiting for:$missing"
	fi
	sleep 1
	waited=$((waited + 1))
done
