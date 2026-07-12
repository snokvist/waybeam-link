#!/usr/bin/env python3
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HTML = (ROOT / "tools" / "link_monitor.html").read_text(encoding="utf-8")


class DashboardSchemaTest(unittest.TestCase):
    def test_frame_shm_counters_are_rendered(self):
        for field in (
            "frames_fast", "recovered_fec", "frames_unrecoverable",
            "malformed", "decode_errors", "dropped_superseded",
            "dropped_deadline", "shm_full_drops", "shm_oversize_drops",
            "shm_bad_slots",
        ):
            self.assertIn(f"s.{field}", HTML)

    def test_adapter_local_drop_counters_are_rendered(self):
        for field in ("drop", "filtered", "kernel_drop"):
            self.assertIn(f"a.{field}", HTML)

    def test_stream_rate_matches_by_identity(self):
        self.assertIn("find(p=>p.stream_id===s.stream_id)", HTML)

    def test_existing_instances_bootstrap_before_sse(self):
        self.assertIn('fetch("/api/instances")', HTML)

    def test_monitor_explains_selector_states_and_metrics(self):
        for text in ("HOLD", "PINNED", "State guide", "data-tip",
                     "Loss before diversity"):
            with self.subTest(text=text):
                self.assertIn(text, HTML)

    def test_monitor_has_metric_tabs_and_live_trends(self):
        for text in ("Overview", "Link & adapters", "Streams", "Frame & SHM",
                     "Trends", "data-chart=", "Encoded frame byte size"):
            with self.subTest(text=text):
                self.assertIn(text, HTML)

    def test_inferred_role_is_not_presented_as_process_mode(self):
        self.assertIn('rec.role==="loopback"?"TX+RX"', HTML)
        self.assertIn("not configured process mode", HTML)


if __name__ == "__main__":
    unittest.main()
