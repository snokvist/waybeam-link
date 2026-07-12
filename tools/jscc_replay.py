#!/usr/bin/env python3
"""Build and replay deterministic JSCC controller traces.

The Ethernet bench has no synchronized capture clock. Its provisional deadline
therefore measures excess inter-arrival time over the run's median cadence, not
sensor-to-display latency. VFRM PTS remains an opaque SDK correlation value.
"""

import argparse
import copy
import csv
import json
import math
import pathlib
import random
import statistics
import sys


SCHEMA = "waybeam-jscc-trace-v1"
PACKET_SCHEMA = "waybeam-packet-events-v1"
DATA_HEADER_BYTES = 26
FEC_REPAIR_HEADER_BYTES = 11
FRAME_META_BYTES = 8
FEC_MAX_SYMBOLS = 256


def nearest_rank(values, quantile):
    """Return the deterministic nearest-rank quantile for integer samples."""
    if not values:
        return 0
    rank = max(1, math.ceil(quantile * len(values)))
    return sorted(values)[rank - 1]


class CausalLossEstimator:
    """Trailing-window empirical loss predictor; observe only after deciding."""
    def __init__(self, window, quantile, min_samples, cold_start):
        if window <= 0:
            raise ValueError("estimator window must be positive")
        if not 0.0 <= quantile <= 1.0:
            raise ValueError("estimator quantile must be in [0, 1]")
        if min_samples < 0 or min_samples > window:
            raise ValueError("estimator min samples must be in [0, window]")
        self.window = window
        self.quantile = quantile
        self.min_samples = min_samples
        self.cold_start = cold_start
        self.samples = []

    def predict(self):
        if len(self.samples) < self.min_samples:
            return self.cold_start
        return nearest_rank(self.samples, self.quantile)

    def observe(self, lost_source_symbols):
        self.samples.append(max(0, int(lost_source_symbols)))
        del self.samples[:-self.window]


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


def read_jsonl(path):
    return [json.loads(line) for line in pathlib.Path(path).read_text(
        encoding="utf-8").splitlines() if line.strip()]


def build_event_trace(args):
    tx_rows = read_jsonl(args.tx_packets)
    rx_rows = read_jsonl(args.rx_packets)
    if not tx_rows or tx_rows[0].get("schema") != PACKET_SCHEMA:
        raise ValueError(f"TX trace must use {PACKET_SCHEMA}")
    if not rx_rows or rx_rows[0].get("schema") != PACKET_SCHEMA:
        raise ValueError(f"RX trace must use {PACKET_SCHEMA}")

    def dropped(rows):
        end = next((row for row in reversed(rows)
                    if row.get("type") == "trace_end"), None)
        return None if end is None else int(end.get("events_dropped", 0))

    tx_dropped, rx_dropped = dropped(tx_rows), dropped(rx_rows)
    if tx_dropped is None or rx_dropped is None:
        raise ValueError("packet trace is incomplete (missing trace_end)")

    blocks = {}
    for row in tx_rows:
        if (row.get("type") != "packet" or row.get("packet") != "data" or
                row.get("direction") != "tx" or row.get("outcome") != "submitted"):
            continue
        key = (int(row["session"]), int(row["stream"]), int(row["block"]))
        block = blocks.setdefault(key, {"packets": {}, "retransmissions": [],
                                        "first_tx_us": int(row["t_us"])})
        block["first_tx_us"] = min(block["first_tx_us"], int(row["t_us"]))
        if row.get("retransmit", False):
            block["retransmissions"].append(dict(row))
        else:
            block["packets"].setdefault(int(row["seq"]), dict(row))
    if not blocks:
        raise ValueError("TX packet trace contains no submitted DATA")

    rx_first = {}
    rx_by_packet = {}
    for row in rx_rows:
        if row.get("type") != "packet" or row.get("packet") != "data":
            continue
        key = (int(row["session"]), int(row["stream"]), int(row["block"]))
        if key not in blocks:
            continue
        rx_first[key] = min(rx_first.get(key, int(row["t_us"])), int(row["t_us"]))
        rx_by_packet.setdefault((key, int(row["seq"])), []).append(row)

    records = [{
        "type": "schema",
        "schema": SCHEMA,
        "source": "packet_events",
        "deadline_model": "receiver_relative",
        "deadline_ms": args.deadline_ms,
        "deadline_note": "RX event offsets are host-local; not cross-host one-way latency",
        "tx_trace_events_dropped": tx_dropped,
        "rx_trace_events_dropped": rx_dropped,
    }]
    for ordinal, key in enumerate(sorted(blocks, key=lambda item: blocks[item]["first_tx_us"])):
        block = blocks[key]
        packets = []
        source_bytes = 0
        k = 0
        arq = False
        for seq, packet in sorted(block["packets"].items(), key=lambda item: item[1]["t_us"]):
            if packet["kind"] == "source":
                source_bytes += max(0, int(packet["bytes"]) - DATA_HEADER_BYTES - 4)
            k = max(k, int(packet.get("k", 0)))
            arq = arq or bool(packet.get("arq", False))
            paths = []
            for event in rx_by_packet.get((key, seq), []):
                paths.append({
                    "adapter": int(event["adapter"]),
                    "outcome": event["outcome"],
                    "arrival_offset_us": int(event["t_us"]) - rx_first[key],
                    "retransmit": bool(event.get("retransmit", False)),
                })
            packets.append({
                "seq": seq,
                "kind": packet["kind"],
                "symbol": int(packet["symbol"]),
                "tx_offset_us": int(packet["t_us"]) - block["first_tx_us"],
                "paths": paths,
            })
        nacks = []
        packet_seqs = set(block["packets"])
        for event in rx_rows:
            if (event.get("type") != "packet" or event.get("packet") != "nack" or
                    event.get("direction") != "tx" or
                    event.get("outcome") != "submitted"):
                continue
            bitmap = bytes.fromhex(event.get("bitmap", ""))
            missing = [int(event["base_seq"]) + bit
                       for bit in range(len(bitmap) * 8)
                       if bitmap[bit // 8] & (1 << (bit % 8))]
            relevant = sorted(packet_seqs.intersection(missing))
            if relevant:
                nacks.append({
                    "offset_us": int(event["t_us"]) - rx_first.get(key, int(event["t_us"])),
                    "base_seq": int(event["base_seq"]),
                    "missing_seq": relevant,
                })
        records.append({
            "type": "block",
            "frame": ordinal,
            "session": key[0],
            "stream": key[1],
            "block": key[2],
            "frame_bytes": source_bytes - FRAME_META_BYTES,
            "source_k": k,
            "parity_m": sum(packet["kind"] == "repair" for packet in packets),
            "arq_eligible": arq,
            "deadline_ms": args.deadline_ms,
            "packets": packets,
            "nacks": nacks,
            "retransmissions": [{
                "seq": int(packet["seq"]),
                "tx_offset_us": int(packet["t_us"]) - block["first_tx_us"],
            } for packet in block["retransmissions"]],
        })
    write_jsonl(args.output, records)
    return records


class LossInjector:
    def __init__(self, args, total_packets, paths):
        self.args = args
        self.total_packets = max(1, total_packets)
        self.paths = paths
        self.rng = [random.Random(args.seed + adapter * 0x9E3779B1)
                    for adapter in range(paths)]
        self.correlated = {}

    def delivered(self, ordinal, adapter):
        if self.args.path_correlation == "correlated":
            if ordinal not in self.correlated:
                self.correlated[ordinal] = self._delivered(ordinal, 0)
            return self.correlated[ordinal]
        return self._delivered(ordinal, adapter)

    def _delivered(self, ordinal, adapter):
        model = self.args.loss_model
        if model == "none":
            return True
        shifted = ordinal + adapter * max(1, self.args.loss_period // self.paths)
        if model == "burst":
            return shifted % self.args.loss_period >= self.args.burst_length
        if model == "low-frequency":
            return shifted % self.args.loss_period != 0
        if model == "incremental":
            fraction = ordinal / max(1, self.total_packets - 1)
            threshold = round(self.args.loss_start_permille +
                              fraction * (self.args.loss_end_permille -
                                          self.args.loss_start_permille))
            return self.rng[adapter].randrange(1000) >= threshold
        if model == "high-frequency":
            return self.rng[adapter].randrange(1000) >= self.args.loss_end_permille
        raise ValueError(f"unknown loss model {model}")


def replay_blocks(records, args):
    blocks = [record for record in records if record.get("type") == "block"]
    deadline_ms = (int(records[0]["deadline_ms"]) if args.deadline_ms is None
                   else args.deadline_ms)
    total_packets = sum(len(block["packets"]) for block in blocks)
    recorded_paths = 1 + max(
        (path["adapter"] for block in blocks for packet in block["packets"]
         for path in packet.get("paths", [])), default=-1)
    paths = args.paths or max(1, recorded_paths)
    injector = LossInjector(args, total_packets, paths)
    estimator = CausalLossEstimator(
        args.estimator_window, args.estimator_quantile,
        args.estimator_min_samples, args.estimator_cold_start)
    global_packet = 0
    decisions = []
    counts = {name: 0 for name in
              ("fast", "fec", "arq", "deadline_discard", "unrecoverable")}
    parity_available_total = 0
    parity_selected_total = 0
    estimator_underpredicted = 0
    for block in blocks:
        predicted_loss = estimator.predict()
        recorded_parity = int(block["parity_m"])
        if args.fec == "adaptive":
            selected_parity = min(
                recorded_parity, args.estimator_cap,
                max(args.estimator_floor, predicted_loss))
        elif args.fec == "on":
            selected_parity = recorded_parity
        else:
            selected_parity = 0
        parity_available_total += recorded_parity
        parity_selected_total += selected_parity
        received = set()
        retransmitted = set()
        arrival_finish_us = 0
        retransmit_finish_us = 0
        any_received = False
        for packet in block["packets"]:
            if args.loss_model == "recorded":
                accepted = [path for path in packet.get("paths", [])
                            if path["outcome"] == "accepted" and
                            not path.get("retransmit", False)]
                accepted_retx = [path for path in packet.get("paths", [])
                                 if path["outcome"] == "accepted" and
                                 path.get("retransmit", False)]
                delivered = bool(accepted)
                if accepted:
                    arrival_finish_us = max(
                        arrival_finish_us,
                        min(int(path["arrival_offset_us"]) for path in accepted))
                if accepted_retx:
                    retransmitted.add((packet["kind"], int(packet["symbol"])))
                    retransmit_finish_us = max(
                        retransmit_finish_us,
                        min(int(path["arrival_offset_us"]) for path in accepted_retx))
            else:
                delivered = any(injector.delivered(global_packet, adapter)
                                for adapter in range(paths))
                if delivered:
                    arrival_finish_us = max(arrival_finish_us,
                                            int(packet["tx_offset_us"]))
            if delivered:
                received.add((packet["kind"], int(packet["symbol"])))
                any_received = True
            global_packet += 1
        k = int(block["source_k"])
        sources = sum(kind == "source" for kind, _ in received)
        received_repairs = sum(kind == "repair" and symbol < selected_parity
                               for kind, symbol in received)
        available = sources + received_repairs
        observed_loss = max(0, k - sources)
        estimator_underpredicted += predicted_loss < observed_loss
        estimator.observe(observed_loss)
        deadline_us = deadline_ms * 1000
        if sources >= k:
            outcome, reason = "fast", "all_sources_received"
        elif selected_parity and available >= k:
            outcome, reason = "fec", "source_plus_repair_rank"
        elif (args.loss_model == "recorded" and retransmitted and
              sum(kind == "source" for kind, _ in received | retransmitted) >= k and
              retransmit_finish_us <= deadline_us):
            outcome, reason = "arq", "recorded_retransmit_completed"
        else:
            eligible = args.arq == "force" or (
                args.arq == "eligible" and block.get("arq_eligible", False))
            arq_finish_us = arrival_finish_us + args.rtt_ms * 1000
            if eligible and any_received and arq_finish_us <= deadline_us:
                outcome, reason = "arq", "rtt_inside_remaining_deadline"
            elif args.deadline_discard == "on":
                outcome, reason = "deadline_discard", (
                    "no_loss_observation" if not any_received else
                    "insufficient_symbols_before_deadline")
            else:
                outcome, reason = "unrecoverable", "recovery_disabled_or_late"
        counts[outcome] += 1
        decisions.append({
            "type": "decision",
            "frame": block["frame"],
            "block": block["block"],
            "source_k": k,
            "received_sources": sources,
            "received_symbols": available,
            "observed_loss_symbols": observed_loss,
            "predicted_loss_symbols": predicted_loss,
            "parity_available_m": recorded_parity,
            "parity_m": selected_parity,
            "outcome": outcome,
            "reason": reason,
        })
    decisions.append({
        "type": "summary",
        "schema": SCHEMA,
        "source": "packet_events",
        "frames": len(blocks),
        "loss_model": args.loss_model,
        "paths": paths,
        "path_correlation": args.path_correlation,
        "fec": args.fec,
        "estimator": ({
            "kind": "trailing_empirical_quantile",
            "window": args.estimator_window,
            "quantile": args.estimator_quantile,
            "min_samples": args.estimator_min_samples,
            "cold_start": args.estimator_cold_start,
            "floor": args.estimator_floor,
            "cap": args.estimator_cap,
        } if args.fec == "adaptive" else None),
        "arq": args.arq,
        "rtt_ms": args.rtt_ms,
        "deadline_ms": deadline_ms,
        "parity_available_symbols": parity_available_total,
        "parity_selected_symbols": parity_selected_total,
        "parity_reduction_permille": (
            round(1000 * (parity_available_total - parity_selected_total) /
                  parity_available_total) if parity_available_total else 0),
        "estimator_underpredicted_blocks": (
            estimator_underpredicted if args.fec == "adaptive" else None),
        **counts,
    })
    return decisions


def replay_matrix(records, args):
    loss_scenarios = (
        ("recorded", "independent"),
        ("none", "independent"),
        ("burst", "independent"),
        ("burst", "correlated"),
        ("incremental", "independent"),
        ("low-frequency", "independent"),
        ("low-frequency", "correlated"),
        ("high-frequency", "independent"),
        ("high-frequency", "correlated"),
    )
    ablations = (
        ("fec_only", "on", "off", "off"),
        ("fec_arq", "on", "eligible", "off"),
        ("fec_arq_discard", "on", "eligible", "on"),
        ("adaptive_fec_arq_discard", "adaptive", "eligible", "on"),
        ("arq_discard", "off", "eligible", "on"),
    )
    results = []
    for loss_model, correlation in loss_scenarios:
        for name, fec, arq, discard in ablations:
            run = copy.copy(args)
            run.loss_model = loss_model
            run.path_correlation = correlation
            run.fec = fec
            run.arq = arq
            run.deadline_discard = discard
            summary = replay_blocks(records, run)[-1]
            summary["scenario"] = f"{loss_model}:{correlation}"
            summary["ablation"] = name
            results.append(summary)
    return results


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


def add_estimator_args(parser):
    parser.add_argument("--estimator-window", type=int, default=120)
    parser.add_argument("--estimator-quantile", type=float, default=0.95)
    parser.add_argument("--estimator-min-samples", type=int, default=20)
    parser.add_argument("--estimator-cold-start", type=int, default=0)
    parser.add_argument("--estimator-floor", type=int, default=0)
    parser.add_argument("--estimator-cap", type=int, default=255)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build", help="convert a finite bench frames.csv to trace v1")
    build.add_argument("--frames", required=True)
    build.add_argument("--config", required=True)
    build.add_argument("--table", required=True)
    build.add_argument("--deadline-ms", type=int, default=16)
    build.add_argument("--output", required=True)
    events = sub.add_parser("build-events", help="group TX/RX packet events into blocks")
    events.add_argument("--tx-packets", required=True)
    events.add_argument("--rx-packets", required=True)
    events.add_argument("--deadline-ms", type=int, default=16)
    events.add_argument("--output", required=True)
    run = sub.add_parser("replay", help="replay a trace deterministically")
    run.add_argument("trace")
    run.add_argument("--deadline-ms", type=int)
    run.add_argument("--output")
    run.add_argument("--loss-model", choices=("recorded", "none", "burst",
                     "incremental", "low-frequency", "high-frequency"),
                     default="recorded")
    run.add_argument("--seed", type=int, default=1)
    run.add_argument("--paths", type=int)
    run.add_argument("--path-correlation", choices=("independent", "correlated"),
                     default="independent")
    run.add_argument("--loss-start-permille", type=int, default=0)
    run.add_argument("--loss-end-permille", type=int, default=100)
    run.add_argument("--loss-period", type=int, default=100)
    run.add_argument("--burst-length", type=int, default=10)
    run.add_argument("--rtt-ms", type=int, default=4)
    run.add_argument("--fec", choices=("on", "off", "adaptive"), default="on")
    run.add_argument("--arq", choices=("off", "eligible", "force"), default="eligible")
    run.add_argument("--deadline-discard", choices=("on", "off"), default="on")
    add_estimator_args(run)
    matrix = sub.add_parser("matrix", help="run the standard loss/ablation matrix")
    matrix.add_argument("trace")
    matrix.add_argument("--deadline-ms", type=int)
    matrix.add_argument("--output", required=True)
    matrix.add_argument("--seed", type=int, default=1)
    matrix.add_argument("--paths", type=int)
    matrix.add_argument("--loss-start-permille", type=int, default=0)
    matrix.add_argument("--loss-end-permille", type=int, default=100)
    matrix.add_argument("--loss-period", type=int, default=100)
    matrix.add_argument("--burst-length", type=int, default=10)
    matrix.add_argument("--rtt-ms", type=int, default=4)
    add_estimator_args(matrix)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        if args.command == "build":
            records = build_trace(args)
            output = replay(records)
        elif args.command == "build-events":
            records = build_event_trace(args)
            output = [{"type": "summary", "schema": SCHEMA,
                       "source": "packet_events",
                       "blocks": len(records) - 1}]
        elif args.command == "replay":
            records = read_trace(args.trace)
            if records[0].get("source") == "packet_events":
                if args.deadline_ms is None:
                    args.deadline_ms = int(records[0]["deadline_ms"])
                output = replay_blocks(records, args)
            else:
                output = replay(records, args.deadline_ms)
        else:
            records = read_trace(args.trace)
            if records[0].get("source") != "packet_events":
                raise ValueError("matrix requires a packet-event trace")
            if args.deadline_ms is None:
                args.deadline_ms = int(records[0]["deadline_ms"])
            output = replay_matrix(records, args)
            pathlib.Path(args.output).write_text(
                json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if getattr(args, "output", None) and args.command == "replay":
            write_jsonl(args.output, output)
        print(json.dumps(output[-1], indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"jscc_replay: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
