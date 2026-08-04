#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-sign a §10.7 uplink artifact after editing its identity fields.

Needed to test the PAIRING gate independently of the integrity check. Editing
`craft_adapter_fingerprint` by hand breaks the CRC-8, so the loader rejects the
file before `uplink_calib_matches()` is ever consulted — which proves the
corruption check works but says nothing about D3. This re-computes the
fingerprint over the same pinned binary canonicalization the C++ writer uses
(io/src/uplink_calib_store.cpp), so the file loads cleanly and the only thing
wrong with it is the pairing.

Mirror of canonical_bytes(): fixed field order, big-endian, length-prefixed
strings, no padding, `t_unix` EXCLUDED (provenance, not identity), and
first_bad_qdb zeroed when absent so the form round-trips.
"""
import json
import sys

SCHEMA = 1


def crc8_dvbs2(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def canonical(a):
    o = bytearray()
    o.append(SCHEMA)
    ident = a["local_adapter_identity"].encode()
    o += len(ident).to_bytes(2, "big") + ident
    o += int(a["craft_originator"]).to_bytes(2, "big")
    o.append(int(a["craft_adapter_fingerprint"]) & 0xFF)
    o += int(a["channel_mhz"]).to_bytes(2, "big")
    o.append(int(a["bw_mhz"]) & 0xFF)
    ps = a["placements"]
    o += len(ps).to_bytes(2, "big")
    for p in ps:
        o.append(int(p["mcs"]) & 0xFF)
        o.append(1 if p["short_gi"] else 0)
        o += (int(p["placement_qdb"]) & 0xFFFFFFFF).to_bytes(4, "big")
        o.append(int(p["placement_rssi_dbm"]) & 0xFF)
        o += (int(p["placement_loss_milli"]) & 0xFFFF).to_bytes(2, "big")
        o += (int(p["last_clean_qdb"]) & 0xFFFFFFFF).to_bytes(4, "big")
        has = p.get("first_bad_qdb") is not None
        o.append(1 if has else 0)
        o += ((int(p["first_bad_qdb"]) & 0xFFFFFFFF) if has else 0).to_bytes(4, "big")
    return bytes(o)


def fingerprint(a):
    fp = crc8_dvbs2(canonical(a))
    return 1 if fp == 0 else fp   # 0 is the "no artifact" sentinel


def main():
    a = json.load(open(sys.argv[1]))
    for kv in sys.argv[3:]:
        k, v = kv.split("=", 1)
        a[k] = int(v) if v.lstrip("-").isdigit() else v
    a["fingerprint"] = fingerprint(a)
    json.dump(a, open(sys.argv[2], "w"), indent=2, sort_keys=True)
    print(f"  wrote {sys.argv[2]} fingerprint={a['fingerprint']}")


if __name__ == "__main__":
    sys.exit(main())
