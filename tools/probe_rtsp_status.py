#!/usr/bin/env python3
"""Send one RTSP request over TCP or TLS and verify the response status."""

from __future__ import annotations

import argparse
import socket
import ssl
import sys
from urllib.parse import urlsplit


def connect(
    scheme: str,
    host: str,
    port: int,
    timeout: float,
    tls_insecure: bool,
    tls_ca: str | None,
) -> socket.socket:
    raw = socket.create_connection((host, port), timeout=timeout)
    if scheme == "rtsp":
        return raw

    context = ssl.create_default_context(cafile=tls_ca)
    if tls_insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    try:
        return context.wrap_socket(raw, server_hostname=host)
    except Exception:
        raw.close()
        raise


def parse_status_line(first_line: str) -> int:
    parts = first_line.split(" ", 2)
    if len(parts) < 2 or not parts[1].isdigit():
        raise RuntimeError(f"invalid RTSP status line: {first_line!r}")
    status = int(parts[1])
    if not 100 <= status <= 599:
        raise RuntimeError(f"RTSP status is out of range: {status}")
    return status


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("uri", help="rtsp:// or rtsps:// URI")
    parser.add_argument("--method", default="DESCRIBE")
    parser.add_argument("--expect-status", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--tls-insecure", action="store_true")
    parser.add_argument("--tls-ca")
    args = parser.parse_args()

    parsed = urlsplit(args.uri)
    if parsed.scheme not in {"rtsp", "rtsps"} or not parsed.hostname:
        parser.error("uri must use rtsp:// or rtsps:// and include a host")
    if args.timeout <= 0 or not 100 <= args.expect_status <= 599:
        parser.error("timeout and expected status are out of range")
    if parsed.scheme == "rtsp" and (args.tls_insecure or args.tls_ca):
        parser.error("TLS options require an rtsps:// URI")
    if args.tls_insecure and args.tls_ca:
        parser.error("--tls-insecure and --tls-ca are mutually exclusive")

    port = parsed.port or (322 if parsed.scheme == "rtsps" else 554)
    with connect(parsed.scheme, parsed.hostname, port, args.timeout, args.tls_insecure, args.tls_ca) as sock:
        sock.settimeout(args.timeout)
        request = (
            f"{args.method} {args.uri} RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Accept: application/sdp\r\n"
            "User-Agent: rpi-vms-status-probe\r\n\r\n"
        ).encode("ascii")
        sock.sendall(request)
        first_line = sock.recv(4096).split(b"\r\n", 1)[0].decode("ascii", errors="replace")

    status = parse_status_line(first_line)
    if status != args.expect_status:
        raise RuntimeError(f"expected RTSP {args.expect_status}, got {status}: {first_line}")
    print(f"PASS: {args.method} {args.uri} returned RTSP {status}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ssl.SSLError, ValueError) as exc:
        print(f"FAIL: RTSP status probe: {exc}", file=sys.stderr)
        sys.exit(1)
