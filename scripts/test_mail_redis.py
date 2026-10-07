#!/usr/bin/env python3
"""Run the C++ mail integration test against a small RESP-compatible test server."""

from __future__ import annotations

import argparse
import socketserver
import subprocess
import threading
import time


class RedisState:
    def __init__(self) -> None:
        self.values: dict[str, tuple[bytes, float]] = {}
        self.zsets: dict[str, dict[str, float]] = {}
        self.lock = threading.Lock()

    def get(self, key: str) -> bytes | None:
        value = self.values.get(key)
        if value is None:
            return None
        data, expires_at = value
        if expires_at <= time.time():
            self.values.pop(key, None)
            return None
        return data


STATE = RedisState()


def simple(value: str) -> bytes:
    return b"+" + value.encode() + b"\r\n"


def error(value: str) -> bytes:
    return b"-ERR " + value.encode() + b"\r\n"


def integer(value: int) -> bytes:
    return f":{value}\r\n".encode()


def bulk(value: bytes | None) -> bytes:
    if value is None:
        return b"$-1\r\n"
    return f"${len(value)}\r\n".encode() + value + b"\r\n"


def array(values: list[str]) -> bytes:
    return f"*{len(values)}\r\n".encode() + b"".join(
        bulk(value.encode()) for value in values
    )


class Handler(socketserver.StreamRequestHandler):
    def read_command(self) -> list[bytes] | None:
        first = self.rfile.readline()
        if not first:
            return None
        if not first.startswith(b"*"):
            raise ValueError("expected array")
        count = int(first[1:-2])
        result = []
        for _ in range(count):
            length_line = self.rfile.readline()
            if not length_line.startswith(b"$"):
                raise ValueError("expected bulk string")
            length = int(length_line[1:-2])
            value = self.rfile.read(length)
            if self.rfile.read(2) != b"\r\n":
                raise ValueError("invalid bulk terminator")
            result.append(value)
        return result

    def handle(self) -> None:
        while True:
            try:
                command = self.read_command()
                if command is None:
                    return
                response = self.execute(command)
            except Exception as exc:  # pragma: no cover - protocol failure path
                response = error(str(exc))
            self.wfile.write(response)
            self.wfile.flush()

    def execute(self, command: list[bytes]) -> bytes:
        name = command[0].decode().upper()
        args = command[1:]
        with STATE.lock:
            if name == "PING":
                return simple("PONG")
            if name in {"AUTH", "SELECT"}:
                return simple("OK")
            if name == "GET" and len(args) == 1:
                return bulk(STATE.get(args[0].decode()))
            if name == "SET" and len(args) == 4:
                key = args[0].decode()
                if args[2].upper() != b"EX" or int(args[3]) != 7200:
                    return error("mail SET must use EX 7200")
                STATE.values[key] = (args[1], time.time() + 7200)
                return simple("OK")
            if name == "SET" and len(args) == 5:
                key = args[0].decode()
                if (
                    args[2].upper() != b"NX"
                    or args[3].upper() != b"EX"
                    or int(args[4]) != 7200
                ):
                    return error("delivery marker must use NX EX 7200")
                if STATE.get(key) is not None:
                    return bulk(None)
                STATE.values[key] = (args[1], time.time() + 7200)
                return simple("OK")
            if name == "DEL" and len(args) == 1:
                existed = STATE.values.pop(args[0].decode(), None) is not None
                return integer(int(existed))
            if name == "ZADD" and len(args) == 3:
                members = STATE.zsets.setdefault(args[0].decode(), {})
                member = args[2].decode()
                added = member not in members
                members[member] = float(args[1])
                return integer(int(added))
            if name == "ZREM" and len(args) == 2:
                members = STATE.zsets.setdefault(args[0].decode(), {})
                removed = members.pop(args[1].decode(), None) is not None
                return integer(int(removed))
            if name == "ZREMRANGEBYSCORE" and len(args) == 3:
                members = STATE.zsets.setdefault(args[0].decode(), {})
                minimum, maximum = float(args[1]), float(args[2])
                targets = [
                    member
                    for member, score in members.items()
                    if minimum <= score <= maximum
                ]
                for member in targets:
                    del members[member]
                return integer(len(targets))
            if name == "ZRANGEBYSCORE" and len(args) == 3:
                members = STATE.zsets.setdefault(args[0].decode(), {})
                minimum = float(args[1])
                maximum = float("inf") if args[2] == b"+inf" else float(args[2])
                result = [
                    member
                    for member, score in sorted(members.items(), key=lambda item: item[1])
                    if minimum <= score <= maximum
                ]
                return array(result)
            return error(f"unsupported command {name}")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()
    with Server(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            return subprocess.run(
                [args.executable, str(server.server_address[1])], check=False
            ).returncode
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
