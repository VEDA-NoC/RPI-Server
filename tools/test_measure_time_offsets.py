#!/usr/bin/env python3

import importlib.util
import io
import unittest
from pathlib import Path
from unittest import mock


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


class NtpSamplingTest(unittest.TestCase):
    def test_parser_defaults_to_minimum_ntp_interval(self):
        parser = MODULE.build_parser()
        args = parser.parse_args(["--ntp-server", "192.0.2.1", "--local-probe"])

        self.assertEqual(args.ntp_interval, MODULE.MIN_NTP_REQUEST_INTERVAL_S)

    def test_waits_configured_interval_between_requests(self):
        sample = {
            "peer": "192.0.2.1",
            "offset_ms": 1.0,
            "round_trip_delay_ms": 2.0,
        }
        with mock.patch.object(
            MODULE, "query_ntp_once", return_value=sample.copy()
        ), mock.patch.object(MODULE.time, "sleep") as sleep, mock.patch.object(
            MODULE.sys, "stderr", io.StringIO()
        ):
            result = MODULE.collect_ntp_samples(
                "192.0.2.1", 3, 1.0, MODULE.MIN_NTP_REQUEST_INTERVAL_S
            )

        self.assertEqual(result["successful_samples"], 3)
        self.assertEqual(
            sleep.call_args_list,
            [
                mock.call(MODULE.MIN_NTP_REQUEST_INTERVAL_S),
                mock.call(MODULE.MIN_NTP_REQUEST_INTERVAL_S),
            ],
        )

    def test_remote_probe_explains_stale_pi_source(self):
        completed = {
            "ok": False,
            "stderr": "error: unrecognized arguments: --ntp-interval 15.0",
        }
        with mock.patch.object(MODULE, "run_command", return_value=completed):
            with self.assertRaisesRegex(RuntimeError, "Synchronize the latest source"):
                MODULE.collect_remote_pi(
                    "noc@noc", "/home/noc/rpi-vms", "192.0.2.1", 3, 1.0, 15.0
                )


if __name__ == "__main__":
    unittest.main()
