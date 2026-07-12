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


if __name__ == "__main__":
    unittest.main()
