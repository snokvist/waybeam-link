#!/usr/bin/env python3
import csv
import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "jscc_replay", ROOT / "tools" / "jscc_replay.py")
jscc_replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jscc_replay)


class JsccReplayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "config.json").write_text(json.dumps({
            "policy": {"select": {"min_profile": 0}},
            "streams": [{"fec": {"scheme": "rlc256", "i_rate_permille": 250,
                                     "p_rate_permille": 100, "min_k": 3}}],
        }), encoding="utf-8")
        (self.root / "table.json").write_text(json.dumps({
            "profiles": [{"id": 0, "max_payload": 1424}],
        }), encoding="utf-8")
        with (self.root / "frames.csv").open("w", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            writer.writerow(("frame", "arrival_ns", "pts", "bytes", "idr"))
            writer.writerow((0, 1_000_000_000, 100, 6400, 0))
            writer.writerow((1, 1_010_000_000, 110, 6400, 0))
            writer.writerow((2, 1_020_000_000, 120, 6400, 0))
            writer.writerow((3, 1_035_000_000, 130, 325000, 0))
        tx = [{"type": "schema", "schema": jscc_replay.PACKET_SCHEMA,
               "role": "tx", "cap": 100}]
        rx = [{"type": "schema", "schema": jscc_replay.PACKET_SCHEMA,
               "role": "rx", "cap": 100}]
        t_us = 1000
        for block in range(2):
            for symbol in range(4):
                kind = "source" if symbol < 3 else "repair"
                seq = block * 4 + symbol
                row = {"type": "packet", "t_us": t_us, "direction": "tx",
                       "outcome": "submitted", "adapter": 0, "packet": "data",
                       "originator": 17, "session": 99, "stream": 0,
                       "block": block, "seq": seq, "kind": kind,
                       "symbol": symbol if kind == "source" else 0, "k": 3,
                       "frame_len": 3000, "arq": True, "retransmit": False,
                       "eob": symbol == 2, "bytes": 1030}
                tx.append(row)
                for adapter in range(2):
                    event = dict(row)
                    event.update(t_us=t_us + 100 + adapter, direction="rx",
                                 adapter=adapter)
                    if block == 1 and symbol == 0:
                        event["outcome"] = "synthetic_drop"
                    else:
                        event["outcome"] = "accepted"
                    rx.append(event)
                t_us += 50
        tx.append({"type": "trace_end", "events": 8, "events_dropped": 0})
        rx.append({"type": "trace_end", "events": 16, "events_dropped": 0})
        for name, rows in (("tx-packets.jsonl", tx), ("rx-packets.jsonl", rx)):
            (self.root / name).write_text(
                "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_build_and_replay_are_deterministic(self):
        class Args:
            frames = self.root / "frames.csv"
            config = self.root / "config.json"
            table = self.root / "table.json"
            deadline_ms = 1
            output = self.root / "trace.jsonl"

        records = jscc_replay.build_trace(Args)
        first = jscc_replay.replay(records)
        second = jscc_replay.replay(jscc_replay.read_trace(Args.output))
        self.assertEqual(first, second)
        self.assertEqual("on_time", first[0]["outcome"])
        self.assertEqual("late", first[3]["outcome"])
        self.assertFalse(records[4]["fec_capacity_ok"])
        self.assertEqual(1, first[-1]["fec_oversize"])
        self.assertLess(first[-1]["fec_capacity_headroom_min"], 0)

    def test_typical_frame_is_well_inside_capacity(self):
        symbol, k, target_m, emitted_m, okay = jscc_replay.allocation(
            204000, False, 1424,
            {"scheme": "rlc256", "i_rate_permille": 250,
             "p_rate_permille": 100, "min_k": 3})
        self.assertEqual(1387, symbol)
        self.assertEqual(148, k)
        self.assertEqual(15, target_m)
        self.assertEqual(target_m, emitted_m)
        self.assertTrue(okay)

    def test_packet_events_replay_fec_and_ablation(self):
        args = jscc_replay.parse_args([
            "build-events", "--tx-packets", str(self.root / "tx-packets.jsonl"),
            "--rx-packets", str(self.root / "rx-packets.jsonl"),
            "--deadline-ms", "16", "--output", str(self.root / "events.jsonl")])
        records = jscc_replay.build_event_trace(args)
        replay_args = jscc_replay.parse_args(["replay", str(self.root / "events.jsonl")])
        decisions = jscc_replay.replay_blocks(records, replay_args)
        self.assertEqual("fast", decisions[0]["outcome"])
        self.assertEqual("fec", decisions[1]["outcome"])
        self.assertEqual(1, decisions[-1]["fast"])
        self.assertEqual(1, decisions[-1]["fec"])

        replay_args.fec = "off"
        replay_args.arq = "off"
        decisions = jscc_replay.replay_blocks(records, replay_args)
        self.assertEqual("deadline_discard", decisions[1]["outcome"])

    def test_seeded_loss_models_are_deterministic(self):
        args = jscc_replay.parse_args([
            "build-events", "--tx-packets", str(self.root / "tx-packets.jsonl"),
            "--rx-packets", str(self.root / "rx-packets.jsonl"),
            "--output", str(self.root / "events.jsonl")])
        records = jscc_replay.build_event_trace(args)
        replay_args = jscc_replay.parse_args([
            "replay", str(self.root / "events.jsonl"), "--loss-model", "incremental",
            "--loss-start-permille", "0", "--loss-end-permille", "900",
            "--seed", "42", "--paths", "2"])
        self.assertEqual(jscc_replay.replay_blocks(records, replay_args),
                         jscc_replay.replay_blocks(records, replay_args))

        replay_args.loss_model = "low-frequency"
        replay_args.loss_period = 2
        replay_args.arq = "off"
        replay_args.path_correlation = "independent"
        independent = jscc_replay.replay_blocks(records, replay_args)[-1]
        replay_args.path_correlation = "correlated"
        correlated = jscc_replay.replay_blocks(records, replay_args)[-1]
        self.assertGreater(independent["fast"], correlated["fast"])

        matrix_args = jscc_replay.parse_args([
            "matrix", str(self.root / "events.jsonl"),
            "--output", str(self.root / "matrix.json")])
        matrix = jscc_replay.replay_matrix(records, matrix_args)
        self.assertEqual(36, len(matrix))
        self.assertEqual("recorded:independent", matrix[0]["scenario"])
        self.assertEqual("fec_only", matrix[0]["ablation"])


if __name__ == "__main__":
    unittest.main()
