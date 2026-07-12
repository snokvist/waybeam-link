#!/usr/bin/env python3
import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "expand_arq_topology", ROOT / "tools" / "expand_arq_topology.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sample():
    return {
        "tx": {"originator": 17, "bind": {"kind": "udp", "listen": "127.0.0.1:5600"}},
        "rx": {"originator": 9, "bind": {"kind": "udp", "send": "127.0.0.1:5700"}},
        "stream": {"stream_id": 3, "stream_type": "RTP"},
        "udp": {"host": "127.0.0.1", "listen_host": "0.0.0.0",
                "downlink_ports": [5801, 5802], "return_port": 5810},
        "common": {"stats": {"hz": 2}},
    }


class ExpandTest(unittest.TestCase):
    def test_reciprocal_pair(self):
        tx, rx = MODULE.expand(sample())
        self.assertEqual(tx["node"]["preferred_originator"], 9)
        self.assertEqual(rx["node"]["preferred_originator"], 17)
        self.assertEqual(tx["air"]["tx"], ["127.0.0.1:5801", "127.0.0.1:5802"])
        self.assertEqual(rx["air"]["rx"], ["0.0.0.0:5801", "0.0.0.0:5802"])
        self.assertEqual(tx["air"]["rx"], ["0.0.0.0:5810"])
        self.assertEqual(rx["air"]["tx"], ["127.0.0.1:5810"])
        self.assertEqual(rx["streams"][0]["originator"], 17)

    def test_rejects_identity_and_port_collisions(self):
        topology = sample()
        topology["rx"]["originator"] = 17
        with self.assertRaisesRegex(ValueError, "must differ"):
            MODULE.expand(topology)
        topology = sample()
        topology["udp"]["return_port"] = 5801
        with self.assertRaisesRegex(ValueError, "collide"):
            MODULE.expand(topology)


if __name__ == "__main__":
    unittest.main()
