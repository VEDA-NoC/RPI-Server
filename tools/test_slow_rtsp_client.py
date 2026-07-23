#!/usr/bin/env python3
"""Open an RTSP/TCP session and deliberately stop reading interleaved RTP."""

from __future__ import annotations

import argparse
import socket
import sys
import time
from urllib.parse import urlsplit


def read_response(sock: socket.socket) -> tuple[int, dict[str, str], bytes]:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("server closed the connection before the RTSP response")
        data.extend(chunk)

    header_data, body = bytes(data).split(b"\r\n\r\n", 1)
    lines = header_data.decode("ascii", errors="replace").split("\r\n")
    parts = lines[0].split(" ", 2)
    if len(parts) < 2 or not parts[1].isdigit():
        raise RuntimeError(f"invalid RTSP status line: {lines[0]!r}")

    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()

    content_length = int(headers.get("content-length", "0"))
    while len(body) < content_length:
        chunk = sock.recv(content_length - len(body))
        if not chunk:
            raise RuntimeError("server closed the connection during the RTSP body")
        body += chunk
    return int(parts[1]), headers, body[:content_length]


def request(
    sock: socket.socket,
    method: str,
    uri: str,
    cseq: int,
    headers: dict[str, str] | None = None,
) -> tuple[dict[str, str], bytes]:
    lines = [f"{method} {uri} RTSP/1.0", f"CSeq: {cseq}", "User-Agent: rpi-vms-slow-client"]
    for name, value in (headers or {}).items():
        lines.append(f"{name}: {value}")
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
    sock.sendall(payload)
    status, response_headers, body = read_response(sock)
    if status != 200:
        raise RuntimeError(f"{method} failed with RTSP status {status}")
    return response_headers, body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("uri", help="RTSP URI, for example rtsp://127.0.0.1:8554/ch1")
    parser.add_argument("--pause-seconds", type=float, default=35.0)
    parser.add_argument("--receive-buffer", type=int, default=1024)
    args = parser.parse_args()

    parsed = urlsplit(args.uri)
    if parsed.scheme != "rtsp" or not parsed.hostname:
        parser.error("uri must be an rtsp:// URI with a host")
    if args.pause_seconds <= 0 or args.receive_buffer <= 0:
        parser.error("pause-seconds and receive-buffer must be positive")

    port = parsed.port or 554
    base_uri = args.uri.rstrip("/")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.receive_buffer)
        sock.settimeout(10.0)
        sock.connect((parsed.hostname, port))

        request(sock, "OPTIONS", base_uri, 1)
        request(sock, "DESCRIBE", base_uri, 2, {"Accept": "application/sdp"})
        setup_headers, _ = request(
            sock,
            "SETUP",
            f"{base_uri}/trackID=0",
            3,
            {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"},
        )
        session = setup_headers.get("session", "").split(";", 1)[0]
        if not session:
            raise RuntimeError("SETUP response did not include a Session header")
        request(sock, "PLAY", base_uri, 4, {"Session": session})

        sock.settimeout(None)
        print(f"PASS: PLAY accepted; pausing reads for {args.pause_seconds:g} seconds", flush=True)
        time.sleep(args.pause_seconds)
        print("PASS: slow client pause completed", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"FAIL: slow RTSP client: {exc}", file=sys.stderr)
        sys.exit(1)
