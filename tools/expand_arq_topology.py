#!/usr/bin/env python3
"""Expand one UDP ARQ topology into matched waybeam-link TX/RX configs."""

import argparse
import copy
import json
import pathlib
import sys


def fail(message):
    raise ValueError(message)


def require_object(value, name):
    if not isinstance(value, dict):
        fail(f"{name} must be an object")
    return value


def require_originator(value, name):
    if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= 65535:
        fail(f"{name} must be an integer in [1, 65535]")
    return value


def require_port(value, name):
    if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= 65535:
        fail(f"{name} must be an integer in [1, 65535]")
    return value


def endpoint(host, port):
    return f"{host}:{port}"


def expand(topology):
    root = require_object(topology, "topology")
    tx_in = require_object(root.get("tx"), "tx")
    rx_in = require_object(root.get("rx"), "rx")
    stream_in = require_object(root.get("stream"), "stream")
    udp = require_object(root.get("udp"), "udp")
    common = require_object(root.get("common", {}), "common")

    tx_id = require_originator(tx_in.get("originator"), "tx.originator")
    rx_id = require_originator(rx_in.get("originator"), "rx.originator")
    if tx_id == rx_id:
        fail("tx.originator and rx.originator must differ")

    host = udp.get("host", "127.0.0.1")
    listen_host = udp.get("listen_host", host)
    if not isinstance(host, str) or not host or not isinstance(listen_host, str) or not listen_host:
        fail("udp.host and udp.listen_host must be non-empty strings")
    ports = udp.get("downlink_ports")
    if not isinstance(ports, list) or not ports:
        fail("udp.downlink_ports must be a non-empty array")
    ports = [require_port(port, "udp.downlink_ports[]") for port in ports]
    return_port = require_port(udp.get("return_port"), "udp.return_port")
    if len(set(ports)) != len(ports):
        fail("udp.downlink_ports must be unique")
    if return_port in ports:
        fail("udp.return_port must not collide with a downlink port")

    stream_id = stream_in.get("stream_id", 0)
    if not isinstance(stream_id, int) or isinstance(stream_id, bool) or not 0 <= stream_id <= 255:
        fail("stream.stream_id must be an integer in [0, 255]")
    stream_type = stream_in.get("stream_type", "RTP")
    if not isinstance(stream_type, str) or not stream_type:
        fail("stream.stream_type must be a non-empty string")
    tx_bind = require_object(tx_in.get("bind"), "tx.bind")
    rx_bind = require_object(rx_in.get("bind"), "rx.bind")

    base = copy.deepcopy(common)
    base.pop("node", None)
    base.pop("streams", None)
    base.pop("air", None)

    tx = copy.deepcopy(base)
    tx["node"] = {"originator": tx_id, "role": "tx", "preferred_originator": rx_id}
    tx_stream = {
        "stream_id": stream_id,
        "stream_type": stream_type,
        "dir": "in",
        "bind": copy.deepcopy(tx_bind),
    }
    if "fec" in stream_in:
        tx_stream["fec"] = copy.deepcopy(require_object(stream_in["fec"], "stream.fec"))
    tx["streams"] = [tx_stream]
    tx["air"] = {
        "kind": "udp",
        "tx": [endpoint(host, port) for port in ports],
        "rx": [endpoint(listen_host, return_port)],
    }

    rx = copy.deepcopy(base)
    rx["node"] = {"originator": rx_id, "role": "rx", "preferred_originator": tx_id}
    rx["streams"] = [{
        "stream_id": stream_id,
        "stream_type": stream_type,
        "dir": "out",
        "originator": tx_id,
        "bind": copy.deepcopy(rx_bind),
    }]
    rx["air"] = {
        "kind": "udp",
        "tx": [endpoint(host, return_port)],
        "rx": [endpoint(listen_host, port) for port in ports],
    }
    return tx, rx


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=pathlib.Path)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        topology = json.loads(args.topology.read_text(encoding="utf-8"))
        tx, rx = expand(topology)
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "tx.json").write_text(json.dumps(tx, indent=2) + "\n", encoding="utf-8")
        (args.out_dir / "rx.json").write_text(json.dumps(rx, indent=2) + "\n", encoding="utf-8")
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"expand_arq_topology: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
