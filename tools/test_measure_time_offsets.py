#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("measure_time_offsets.py")
SPEC = importlib.util.spec_from_file_location("measure_time_offsets", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SunapiResponseTest(unittest.TestCase):
    def test_parses_key_value_response(self):
        parsed = MODULE.parse_sunapi_date_response(
            "\n".join(
                [
                    "LocalTime=2026-07-16 11:32:15",
                    "UTCTime=2026-07-16 02:32:15",
                    "SyncType=Manual",
                    "NTPURLList=pool.ntp.org,asia.pool.ntp.org",
                ]
            )
        )
        self.assertEqual(parsed["UTCTime"], "2026-07-16 02:32:15")
        self.assertEqual(parsed["SyncType"], "Manual")

    def test_parses_nested_json_response(self):
        parsed = MODULE.parse_sunapi_date_response(
            '{"Date": {"UTCTime": "2026-07-16 02:32:15", '
            '"SyncType": "NTP", "NTPURLList": ["pool.ntp.org", '
            '"asia.pool.ntp.org"]}}'
        )
        self.assertEqual(parsed["UTCTime"], "2026-07-16 02:32:15")
        self.assertEqual(parsed["SyncType"], "NTP")
        self.assertEqual(
            parsed["NTPURLList"], ["pool.ntp.org", "asia.pool.ntp.org"]
        )

    def test_rejects_response_without_utc(self):
        with self.assertRaises(ValueError):
            MODULE.parse_sunapi_date_response("SyncType=Manual")


class OffsetCalculationTest(unittest.TestCase):
    def test_relative_offset_signs(self):
        windows = {
            "ntp": {
                "offset_ms": {"median": 20.0},
                "round_trip_delay_ms": {"median": 4.0},
                "samples": [{"peer": "192.0.2.1"}],
            }
        }
        pi = {
            "ntp": {
                "offset_ms": {"median": -5.0},
                "round_trip_delay_ms": {"median": 6.0},
                "samples": [{"peer": "192.0.2.1"}],
            }
        }
        camera = {
            "camera_minus_local_ms": {"median": 100.0},
            "samples": [{"uncertainty_ms": 510.0}],
        }
        result = MODULE.calculate_relative_offsets(windows, pi, camera)
        self.assertEqual(result["pi_minus_windows_ms"], 25.0)
        self.assertEqual(result["camera_minus_reference_ms"], 80.0)
        self.assertEqual(result["camera_minus_pi_ms"], 75.0)
        self.assertEqual(
            result["estimated_uncertainty_ms"]["pi_minus_windows"], 5.0
        )
        self.assertTrue(result["reference_peers"]["same_address_set"])


if __name__ == "__main__":
    unittest.main()
