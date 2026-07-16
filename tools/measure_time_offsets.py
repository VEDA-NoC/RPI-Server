#!/usr/bin/env python3
"""Collect read-only camera/Pi/Windows clock state and estimate offsets."""

from __future__ import annotations

import argparse
import datetime as dt
import getpass
import json
import locale
import os
import platform
import shlex
import socket
import statistics
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any


NTP_EPOCH_DELTA = 2_208_988_800
SUNAPI_DATE_PATH = "/stw-cgi/system.cgi?msubmenu=date&action=view"
CAMERA_TIME_RESOLUTION_MS = 1_000.0


def utc_now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds")


def run_command(command: list[str], timeout_s: float = 15.0) -> dict[str, Any]:
    command_encoding = (
        "cp949" if os.name == "nt" else locale.getpreferredencoding(False)
    )
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding=command_encoding,
            errors="replace",
            timeout=timeout_s,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as error:
        return {"command": command, "ok": False, "error": str(error)}

    return {
        "command": command,
        "ok": completed.returncode == 0,
        "returncode": completed.returncode,
        "stdout": completed.stdout.rstrip(),
        "stderr": completed.stderr.rstrip(),
    }


def collect_host_status() -> dict[str, Any]:
    status: dict[str, Any] = {
        "collected_at_utc": utc_now_iso(),
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
    }
    if os.name == "nt":
        status["windows_time_status"] = run_command(
            ["w32tm", "/query", "/status", "/verbose"]
        )
        status["windows_time_configuration"] = run_command(
            ["w32tm", "/query", "/configuration"]
        )
        return status

    script = Path(__file__).with_name("check-time-sync.sh")
    if script.exists():
        status["linux_time_status"] = run_command(["bash", str(script)])
    else:
        status["linux_time_status"] = run_command(["timedatectl", "status"])
    return status


def ntp_timestamp_to_unix(seconds: int, fraction: int) -> float:
    return seconds - NTP_EPOCH_DELTA + fraction / 2**32


def query_ntp_once(server: str, timeout_s: float) -> dict[str, Any]:
    request = bytearray(48)
    request[0] = 0x23  # LI=0, VN=4, mode=3 (client)
    addresses = socket.getaddrinfo(server, 123, type=socket.SOCK_DGRAM)
    if not addresses:
        raise RuntimeError(f"NTP server address not found: {server}")

    last_error: Exception | None = None
    for family, socktype, protocol, _, sockaddr in addresses:
        try:
            with socket.socket(family, socktype, protocol) as client:
                client.settimeout(timeout_s)
                t1 = time.time()
                client.sendto(request, sockaddr)
                response, peer = client.recvfrom(512)
                t4 = time.time()
            if len(response) < 48:
                raise RuntimeError(f"short NTP response: {len(response)} bytes")

            fields = struct.unpack("!12I", response[:48])
            t2 = ntp_timestamp_to_unix(fields[8], fields[9])
            t3 = ntp_timestamp_to_unix(fields[10], fields[11])
            offset_s = ((t2 - t1) + (t3 - t4)) / 2.0
            delay_s = (t4 - t1) - (t3 - t2)
            stratum = response[1]
            mode = response[0] & 0x07
            if mode not in (4, 5):
                raise RuntimeError(f"unexpected NTP response mode: {mode}")
            if stratum == 0:
                raise RuntimeError("NTP server returned kiss-of-death/unsynchronized")

            return {
                "peer": str(peer[0]),
                "stratum": stratum,
                "offset_ms": offset_s * 1_000.0,
                "round_trip_delay_ms": max(delay_s, 0.0) * 1_000.0,
                "t1_utc": dt.datetime.fromtimestamp(
                    t1, dt.timezone.utc
                ).isoformat(timespec="microseconds"),
                "t4_utc": dt.datetime.fromtimestamp(
                    t4, dt.timezone.utc
                ).isoformat(timespec="microseconds"),
            }
        except (OSError, RuntimeError) as error:
            last_error = error
    raise RuntimeError(f"NTP query failed for {server}: {last_error}")


def summarize_numeric(samples: list[dict[str, Any]], key: str) -> dict[str, float]:
    values = [float(sample[key]) for sample in samples]
    return {
        "min": min(values),
        "median": statistics.median(values),
        "max": max(values),
    }


def collect_ntp_samples(
    server: str, sample_count: int, timeout_s: float
) -> dict[str, Any]:
    samples: list[dict[str, Any]] = []
    errors: list[str] = []
    for index in range(sample_count):
        try:
            sample = query_ntp_once(server, timeout_s)
            sample["sample"] = index + 1
            samples.append(sample)
        except (OSError, RuntimeError) as error:
            errors.append(str(error))
        if index + 1 < sample_count:
            time.sleep(0.2)

    result: dict[str, Any] = {
        "server": server,
        "requested_samples": sample_count,
        "successful_samples": len(samples),
        "samples": samples,
        "errors": errors,
        "offset_definition": "reference_clock_minus_local_clock",
    }
    if samples:
        result["offset_ms"] = summarize_numeric(samples, "offset_ms")
        result["round_trip_delay_ms"] = summarize_numeric(
            samples, "round_trip_delay_ms"
        )
    return result


def flatten_mapping(value: Any) -> dict[str, Any]:
    flattened: dict[str, Any] = {}

    def visit(item: Any) -> None:
        if isinstance(item, dict):
            for key, child in item.items():
                if isinstance(child, (dict, list)):
                    visit(child)
                else:
                    flattened[str(key).lower()] = child
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return flattened


def parse_sunapi_date_response(body: str) -> dict[str, Any]:
    wanted = {
        "localtime": "LocalTime",
        "utctime": "UTCTime",
        "synctype": "SyncType",
        "dstenable": "DSTEnable",
        "timezoneindex": "TimeZoneIndex",
        "posixtimezone": "POSIXTimeZone",
        "ntpurllist": "NTPURLList",
        "ntpstatus": "NTPStatus",
        "ntplastupdatedtime": "NTPLastUpdatedTime",
    }
    try:
        values = flatten_mapping(json.loads(body))
    except json.JSONDecodeError:
        values = {}
        for line in body.splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip().lower()] = value.strip()

    parsed = {
        output_key: values[input_key]
        for input_key, output_key in wanted.items()
        if input_key in values
    }
    if "UTCTime" not in parsed:
        raise ValueError("SUNAPI response does not contain UTCTime")
    return parsed


def parse_camera_utc(value: Any) -> dt.datetime:
    text = str(value).strip()
    for time_format in (
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%dT%H:%M:%SZ",
    ):
        try:
            return dt.datetime.strptime(text, time_format).replace(
                tzinfo=dt.timezone.utc
            )
        except ValueError:
            pass
    try:
        parsed = dt.datetime.fromisoformat(text)
    except ValueError as error:
        raise ValueError(f"unsupported camera UTCTime: {text}") from error
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def make_digest_opener(
    camera_url: str, username: str, password: str
) -> urllib.request.OpenerDirector:
    password_manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    password_manager.add_password(None, camera_url, username, password)
    return urllib.request.build_opener(
        urllib.request.HTTPDigestAuthHandler(password_manager)
    )


def collect_camera_samples(
    camera_host: str,
    camera_user: str,
    camera_password: str,
    camera_scheme: str,
    sample_count: int,
    timeout_s: float,
) -> dict[str, Any]:
    camera_url = f"{camera_scheme}://{camera_host}{SUNAPI_DATE_PATH}"
    opener = make_digest_opener(camera_url, camera_user, camera_password)
    samples: list[dict[str, Any]] = []
    settings: dict[str, Any] | None = None
    content_type = ""

    for index in range(sample_count):
        request = urllib.request.Request(
            camera_url,
            headers={"Accept": "application/json"},
            method="GET",
        )
        t1 = time.time()
        with opener.open(request, timeout=timeout_s) as response:
            body = response.read().decode("utf-8", errors="replace")
            content_type = response.headers.get("Content-Type", "")
        t4 = time.time()

        parsed = parse_sunapi_date_response(body)
        camera_utc = parse_camera_utc(parsed["UTCTime"]).timestamp()
        midpoint = (t1 + t4) / 2.0
        round_trip_ms = (t4 - t1) * 1_000.0
        samples.append(
            {
                "sample": index + 1,
                "camera_utc": dt.datetime.fromtimestamp(
                    camera_utc, dt.timezone.utc
                ).isoformat(),
                "local_midpoint_utc": dt.datetime.fromtimestamp(
                    midpoint, dt.timezone.utc
                ).isoformat(timespec="microseconds"),
                "camera_minus_local_ms": (camera_utc - midpoint) * 1_000.0,
                "round_trip_ms": round_trip_ms,
                "uncertainty_ms": CAMERA_TIME_RESOLUTION_MS / 2.0
                + round_trip_ms / 2.0,
            }
        )
        settings = parsed
        if index + 1 < sample_count:
            time.sleep(1.05)

    return {
        "endpoint": SUNAPI_DATE_PATH,
        "http_method": "GET",
        "read_only": True,
        "content_type": content_type,
        "settings": settings,
        "samples": samples,
        "camera_minus_local_ms": summarize_numeric(
            samples, "camera_minus_local_ms"
        ),
        "round_trip_ms": summarize_numeric(samples, "round_trip_ms"),
        "uncertainty_note": (
            "SUNAPI UTCTime is observed at one-second resolution; each sample "
            "includes +/-500 ms plus half HTTP round-trip time."
        ),
    }


def collect_remote_pi(
    pi_target: str,
    pi_repo: str,
    ntp_server: str,
    sample_count: int,
    timeout_s: float,
) -> dict[str, Any]:
    remote_script = f"{pi_repo.rstrip('/')}/tools/measure_time_offsets.py"
    remote_command = " ".join(
        shlex.quote(argument)
        for argument in [
            "python3",
            remote_script,
            "--local-probe",
            "--ntp-server",
            ntp_server,
            "--samples",
            str(sample_count),
            "--timeout",
            str(timeout_s),
        ]
    )
    completed = run_command(
        ["ssh", pi_target, remote_command],
        timeout_s=max(30.0, sample_count * (timeout_s + 1.0)),
    )
    if not completed.get("ok"):
        raise RuntimeError(
            "Pi probe failed: "
            + (completed.get("stderr") or completed.get("error") or "unknown error")
        )
    try:
        return json.loads(completed["stdout"])
    except json.JSONDecodeError as error:
        raise RuntimeError("Pi probe did not return valid JSON") from error


def median_from(result: dict[str, Any], section: str, key: str) -> float:
    return float(result[section][key]["median"])


def calculate_relative_offsets(
    windows_probe: dict[str, Any],
    pi_probe: dict[str, Any],
    camera: dict[str, Any],
) -> dict[str, Any]:
    windows_theta = median_from(windows_probe, "ntp", "offset_ms")
    pi_theta = median_from(pi_probe, "ntp", "offset_ms")
    camera_minus_windows = float(camera["camera_minus_local_ms"]["median"])
    pi_minus_windows = windows_theta - pi_theta

    windows_delay = median_from(
        windows_probe, "ntp", "round_trip_delay_ms"
    )
    pi_delay = median_from(pi_probe, "ntp", "round_trip_delay_ms")
    camera_uncertainty = statistics.median(
        float(sample["uncertainty_ms"]) for sample in camera["samples"]
    )
    windows_peers = sorted(
        {str(sample["peer"]) for sample in windows_probe["ntp"]["samples"]}
    )
    pi_peers = sorted(
        {str(sample["peer"]) for sample in pi_probe["ntp"]["samples"]}
    )
    result = {
        "offset_definition": "left_clock_minus_right_clock",
        "pi_minus_windows_ms": pi_minus_windows,
        "camera_minus_windows_ms": camera_minus_windows,
        "camera_minus_reference_ms": camera_minus_windows - windows_theta,
        "camera_minus_pi_ms": camera_minus_windows - pi_minus_windows,
        "estimated_uncertainty_ms": {
            "pi_minus_windows": (windows_delay + pi_delay) / 2.0,
            "camera_minus_reference": camera_uncertainty
            + windows_delay / 2.0,
            "camera_minus_pi": camera_uncertainty
            + (windows_delay + pi_delay) / 2.0,
        },
        "interpretation": (
            "A positive value means the left-side clock is ahead of the "
            "right-side clock."
        ),
        "reference_peers": {
            "windows": windows_peers,
            "raspberry_pi": pi_peers,
            "same_address_set": windows_peers == pi_peers,
        },
    }
    if windows_peers != pi_peers:
        result["warning"] = (
            "The NTP name resolved to different peer address sets. Use one "
            "explicit server IP when tighter cross-device comparison is needed."
        )
    return result


def local_probe(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "host": collect_host_status(),
        "ntp": collect_ntp_samples(
            args.ntp_server, args.samples, args.timeout
        ),
    }


def ensure_ntp_success(probe: dict[str, Any], label: str) -> None:
    if probe["ntp"]["successful_samples"] == 0:
        errors = "; ".join(probe["ntp"]["errors"])
        raise RuntimeError(f"{label} NTP measurement has no valid samples: {errors}")


def default_output_path() -> Path:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return Path("measurements") / f"time-offset-{timestamp}.json"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read camera/Pi/Windows clock state and measure offsets against "
            "one common NTP server without changing device settings."
        )
    )
    parser.add_argument("--camera-host", help="Camera HOST or HOST:PORT")
    parser.add_argument("--camera-user", default="admin")
    parser.add_argument(
        "--camera-scheme", choices=("http", "https"), default="http"
    )
    parser.add_argument("--pi-target", default="noc@noc")
    parser.add_argument("--pi-repo", default="/home/noc/rpi-vms")
    parser.add_argument("--ntp-server", required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--local-probe", action="store_true", help=argparse.SUPPRESS)
    return parser


def validate_args(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if args.samples < 1:
        parser.error("--samples must be at least 1")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not args.local_probe and not args.camera_host:
        parser.error("--camera-host is required")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    validate_args(args, parser)
    if args.local_probe:
        print(json.dumps(local_probe(args), ensure_ascii=False, indent=2))
        return 0
    if os.name != "nt":
        print(
            "error: the full collector must run on Windows; use --local-probe "
            "for Linux/Pi",
            file=sys.stderr,
        )
        return 2

    camera_password = getpass.getpass(
        f"SUNAPI password for {args.camera_user}@{args.camera_host}: "
    )
    result: dict[str, Any] = {
        "schema_version": 1,
        "collected_at_utc": utc_now_iso(),
        "read_only": True,
        "reference_ntp_server": args.ntp_server,
    }
    try:
        result["windows"] = local_probe(args)
        ensure_ntp_success(result["windows"], "Windows")
        result["raspberry_pi"] = collect_remote_pi(
            args.pi_target,
            args.pi_repo,
            args.ntp_server,
            args.samples,
            args.timeout,
        )
        ensure_ntp_success(result["raspberry_pi"], "Raspberry Pi")
        result["camera"] = collect_camera_samples(
            args.camera_host,
            args.camera_user,
            camera_password,
            args.camera_scheme,
            args.samples,
            args.timeout,
        )
        result["relative_offsets"] = calculate_relative_offsets(
            result["windows"], result["raspberry_pi"], result["camera"]
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        camera_password = ""

    output_path = args.output or default_output_path()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"measurement written: {output_path}")
    print(json.dumps(result["relative_offsets"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
