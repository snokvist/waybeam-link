#!/usr/bin/env python3
import pathlib
import importlib.util
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HTML = (ROOT / "tools" / "link_monitor.html").read_text(encoding="utf-8")
spec = importlib.util.spec_from_file_location(
    "link_monitor", ROOT / "tools" / "link_monitor.py")
link_monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link_monitor)


class DashboardSchemaTest(unittest.TestCase):
    def test_frame_shm_counters_are_rendered(self):
        for field in (
            "frames_fast", "recovered_fec", "frames_unrecoverable",
            "malformed", "decode_errors", "dropped_superseded",
            "dropped_deadline", "shm_full_drops", "shm_oversize_drops",
            "shm_bad_slots", "frame_count", "frame_bytes", "frame_size_last",
            "frame_size_min", "frame_size_max", "frame_interval_us",
            "frame_jitter_us",
            "jscc_shadow_blocks", "jscc_predicted_loss_symbols",
            "jscc_observed_loss_symbols", "jscc_underpredicted_blocks",
            "jscc_predicted_parity_symbols",
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
                     "Trends", "data-chart=", "Encoded frame size",
                     "Frame arrival cadence"):
            with self.subTest(text=text):
                self.assertIn(text, HTML)

    def test_inferred_role_is_not_presented_as_process_mode(self):
        self.assertIn('rec.role==="loopback"?"TX+RX"', HTML)
        self.assertIn("not configured process mode", HTML)

    def test_interaction_does_not_rebuild_on_liveness_ticks(self):
        self.assertIn('setInterval(updateLiveness,250)', HTML)
        self.assertNotIn('setInterval(render,1000)', HTML)
        self.assertNotIn('pointerenter', HTML)
        self.assertNotIn('focusin', HTML)
        self.assertIn('pointerover', HTML)
        self.assertIn('lockReason="tooltip"', HTML)
        self.assertIn('unlock(st,"tab")', HTML)
        self.assertIn('deliveryRates', HTML)

    def test_bridge_evicts_superseded_and_stale_sessions(self):
        fleet = link_monitor.Fleet({})
        fleet.ingest("192.0.2.1", '{"t_ms":1,"node":9,"session":1}')
        fleet.ingest("192.0.2.1", '{"t_ms":2,"node":9,"session":2}')
        self.assertEqual([2], [r["snap"]["session"] for r in fleet.snapshot_all()])
        with fleet._lock:
            next(iter(fleet._instances.values()))["recv_wall"] -= 31
        self.assertEqual([], fleet.snapshot_all())
        self.assertIn("instances.delete(k)", HTML)


if __name__ == "__main__":
    unittest.main()
