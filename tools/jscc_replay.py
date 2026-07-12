#!/usr/bin/env python3
"""Build and replay deterministic JSCC controller traces.

The Ethernet bench has no synchronized capture clock. Its provisional deadline
therefore measures excess inter-arrival time over the run's median cadence, not
sensor-to-display latency. VFRM PTS remains an opaque SDK correlation value.
"""

import argparse
import csv
import json
import math
import pathlib
import statistics
import sys


SCHEMA = "waybeam-jscc-trace-v1"
DATA_HEADER_BYTES = 26
FEC_REPAIR_HEADER_BYTES = 11
FRAME_META_BYTES = 8
FEC_MAX_SYMBOLS = 256


def load_json(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def profile_max_payload(config, table):
    profile_id = config.get("policy", {}).get("select", {}).get("min_profile", 0)
    profiles = load_json(table).get("profiles", [])
    profile = next((p for p in profiles if p.get("id") == profile_id), None)
    if profile is None:
        raise ValueError(f"profile {profile_id} not found in {table}")
    return int(profile.get("max_payload", 1424)), profile_id


def fec_config(config):
    stream = config["streams"][0]
    fec = stream.get("fec", {"scheme": "none"})
    return {
        "scheme": fec.get("scheme", "none"),
        "i_rate_permille": int(fec.get("i_rate_permille", 0)),
        "p_rate_permille": int(fec.get("p_rate_permille", 0)),
        "min_k": int(fec.get("min_k", 0)),
    }


def allocation(frame_bytes, idr, max_payload, fec):
    symbol_bytes = max_payload - DATA_HEADER_BYTES - FEC_REPAIR_HEADER_BYTES
    if symbol_bytes <= 0:
        raise ValueError("max_payload leaves no FEC symbol payload")
    wire_frame_bytes = frame_bytes + FRAME_META_BYTES
    k = max(1, math.ceil(wire_frame_bytes / symbol_bytes))
    rate = fec["i_rate_permille"] if idr else fec["p_rate_permille"]
    if fec["scheme"] != "rlc256" or k <= fec["min_k"] or rate == 0:
        target_m = 0
    else:
        target_m = math.ceil(k * rate / 1000)
    capacity_ok = k + target_m <= FEC_MAX_SYMBOLS
    return symbol_bytes, k, target_m, target_m if capacity_ok else 0, capacity_ok


def build_trace(args):
    with pathlib.Path(args.frames).open(encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("frame trace is empty")
    config = load_json(args.config)
    table = args.table
    max_payload, profile_id = profile_max_payload(config, table)
    fec = fec_config(config)
    arrivals = [int(row["arrival_ns"]) for row in rows]
    gaps_us = [(right - left) // 1000 for left, right in zip(arrivals, arrivals[1:])]
    nominal_interval_us = round(statistics.median(gaps_us)) if gaps_us else 0
    header = {
        "type": "schema",
        "schema": SCHEMA,
        "deadline_model": "interarrival_excess",
        "deadline_ms": args.deadline_ms,
        "deadline_note": "gap above run median arrival interval; not one-way latency",
        "nominal_interval_us": nominal_interval_us,
        "profile_id": profile_id,
        "max_payload": max_payload,
        "fec": fec,
    }
    records = [header]
    previous_arrival = None
    for index, row in enumerate(rows):
        arrival_ns = int(row["arrival_ns"])
        arrival_gap_us = ((arrival_ns - previous_arrival) // 1000
                          if previous_arrival is not None else 0)
        relative_delay_us = max(0, arrival_gap_us - nominal_interval_us)
        previous_arrival = arrival_ns
        frame_bytes = int(row["bytes"])
        idr = bool(int(row["idr"]))
        symbol_bytes, k, target_m, emitted_m, capacity_ok = allocation(
            frame_bytes, idr, max_payload, fec)
        records.append({
            "type": "frame",
            "frame": index,
            "pts_sdk": int(row["pts"]),
            "arrival_ns": arrival_ns,
            "arrival_gap_us": arrival_gap_us,
            "frame_bytes": frame_bytes,
            "idr": idr,
            "relative_delay_us": relative_delay_us,
            "deadline_ms": args.deadline_ms,
            "path_delivery": None,
            "rtt_ms": None,
            "symbol_bytes": symbol_bytes,
            "source_k": k,
            "parity_target_m": target_m,
            "parity_emitted_m": emitted_m,
            "fec_capacity_ok": capacity_ok,
        })
    write_jsonl(args.output, records)
    return records


def read_trace(path):
    records = [json.loads(line) for line in pathlib.Path(path).read_text(
        encoding="utf-8").splitlines() if line.strip()]
    if not records or records[0].get("schema") != SCHEMA:
        raise ValueError(f"expected {SCHEMA}")
    return records


def replay(records, deadline_override=None):
    header = records[0]
    deadline_ms = header["deadline_ms"] if deadline_override is None else deadline_override
    decisions = []
    counts = {"on_time": 0, "late": 0, "fec_oversize": 0}
    sizes = []
    max_source_k = 0
    min_capacity_headroom = FEC_MAX_SYMBOLS
    for frame in records[1:]:
        if frame.get("type") != "frame":
            continue
        relative_delay_us = int(frame["relative_delay_us"])
        outcome = "on_time" if relative_delay_us <= deadline_ms * 1000 else "late"
        counts[outcome] += 1
        counts["fec_oversize"] += not frame["fec_capacity_ok"]
        sizes.append(int(frame["frame_bytes"]))
        max_source_k = max(max_source_k, int(frame["source_k"]))
        min_capacity_headroom = min(
            min_capacity_headroom,
            FEC_MAX_SYMBOLS - int(frame["source_k"]) - int(frame["parity_target_m"]))
        decisions.append({
            "type": "decision",
            "frame": frame["frame"],
            "outcome": outcome,
            "deadline_ms": deadline_ms,
            "relative_delay_us": relative_delay_us,
            "source_k": frame["source_k"],
            "parity_m": frame["parity_emitted_m"],
            "fec_capacity_ok": frame["fec_capacity_ok"],
            "reason": "cadence_deadline_met" if outcome == "on_time" else "cadence_deadline_missed",
        })
    summary = {
        "type": "summary",
        "schema": SCHEMA,
        "deadline_model": header["deadline_model"],
        "deadline_ms": deadline_ms,
        "frames": len(decisions),
        **counts,
        "frame_bytes_max": max(sizes) if sizes else 0,
        "source_k_max": max_source_k,
        "fec_capacity_headroom_min": min_capacity_headroom if sizes else 0,
        "shm_slot_limit_bytes": 512 * 1024,
    }
    return decisions + [summary]


def write_jsonl(path, records):
    text = "".join(json.dumps(record, sort_keys=True) + "\n" for record in records)
    pathlib.Path(path).write_text(text, encoding="utf-8")


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build", help="convert a finite bench frames.csv to trace v1")
    build.add_argument("--frames", required=True)
    build.add_argument("--config", required=True)
    build.add_argument("--table", required=True)
    build.add_argument("--deadline-ms", type=int, default=16)
    build.add_argument("--output", required=True)
    run = sub.add_parser("replay", help="replay a trace deterministically")
    run.add_argument("trace")
    run.add_argument("--deadline-ms", type=int)
    run.add_argument("--output")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        if args.command == "build":
            records = build_trace(args)
            output = replay(records)
        else:
            output = replay(read_trace(args.trace), args.deadline_ms)
        if getattr(args, "output", None) and args.command == "replay":
            write_jsonl(args.output, output)
        print(json.dumps(output[-1], indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"jscc_replay: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
