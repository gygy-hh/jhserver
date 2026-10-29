#!/usr/bin/env python3
"""Minimal push port smoke test: auth fail without session, then optional real session.

Usage:
  python3 scripts/test_push_frame.py [--host 127.0.0.1] [--port 18081]
  python3 scripts/test_push_frame.py --session <64hex>
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys


def send_frame(sock: socket.socket, obj: dict) -> None:
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    sock.sendall(struct.pack(">I", len(body)) + body)


def recv_frame(sock: socket.socket) -> dict:
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise ConnectionError("closed while reading header")
        header += chunk
    (n,) = struct.unpack(">I", header)
    if n == 0 or n > 64 * 1024:
        raise ValueError(f"bad frame length {n}")
    body = b""
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        if not chunk:
            raise ConnectionError("closed while reading body")
        body += chunk
    return json.loads(body.decode("utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=18081)
    ap.add_argument("--session", default="")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.settimeout(5)
    try:
        if args.session:
            send_frame(sock, {"type": "auth", "session": args.session})
            msg = recv_frame(sock)
            print("auth response:", msg)
            if msg.get("type") != "auth_ok":
                return 1
            send_frame(sock, {"type": "ping"})
            print("pong:", recv_frame(sock))
            print("ok: connected; leave socket open 3s then exit")
            import time

            time.sleep(3)
            return 0

        send_frame(sock, {"type": "ping"})
        msg = recv_frame(sock)
        print("no-auth response:", msg)
        return 0 if msg.get("type") == "auth_fail" else 1
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
