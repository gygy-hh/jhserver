#!/usr/bin/env python3
"""Push port concurrency stress client (pairs with tests/push_stress_server.cpp).

Opens N long connections at a given rate, auths each with a unique fake session,
pings every 20s like the injected SO, and measures broadcast delivery latency.

  python3 scripts/push_stress_client.py --port 28081 --conns 2000 --rate 200 --hold 40
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import resource
import statistics
import struct
import time


class Stats:
    def __init__(self) -> None:
        self.connected = 0
        self.authed = 0
        self.connect_fail = 0
        self.auth_fail = 0
        self.dropped = 0
        self.auth_ms: list[float] = []
        self.push_ms: list[float] = []
        self.pushes = 0


async def read_frame(reader: asyncio.StreamReader) -> dict:
    header = await reader.readexactly(4)
    (n,) = struct.unpack(">I", header)
    return json.loads(await reader.readexactly(n))


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return struct.pack(">I", len(body)) + body


async def client(i: int, args, stats: Stats, stop: asyncio.Event) -> None:
    session = f"{i:012d}" + os.urandom(26).hex()
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(args.host, args.port), timeout=10)
    except Exception:
        stats.connect_fail += 1
        return
    stats.connected += 1
    authed = False
    try:
        t0 = time.perf_counter()
        writer.write(frame({"type": "auth", "session": session}))
        await writer.drain()
        msg = await asyncio.wait_for(read_frame(reader), timeout=15)
        if msg.get("type") != "auth_ok":
            stats.auth_fail += 1
            return
        stats.authed += 1
        authed = True
        stats.auth_ms.append((time.perf_counter() - t0) * 1000)

        next_ping = time.monotonic() + 20
        while not stop.is_set():
            timeout = max(0.1, next_ping - time.monotonic())
            try:
                msg = await asyncio.wait_for(read_frame(reader), timeout=timeout)
            except asyncio.TimeoutError:
                writer.write(frame({"type": "ping"}))
                await writer.drain()
                next_ping = time.monotonic() + 20
                continue
            if msg.get("type") == "push":
                stats.pushes += 1
                text = msg.get("msg", "")
                if text.startswith("ts="):
                    stats.push_ms.append(time.time() * 1000 - int(text[3:]))
                writer.write(frame({"type": "ack", "id": msg.get("id", "")}))
                await writer.drain()
    except Exception:
        if stop.is_set():
            pass
        elif authed:
            stats.dropped += 1
        else:
            stats.auth_fail += 1
    finally:
        writer.close()


def pct(values: list[float], p: float) -> float:
    if not values:
        return float("nan")
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))]


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=28081)
    ap.add_argument("--conns", type=int, default=1000)
    ap.add_argument("--rate", type=int, default=200, help="new connections per second")
    ap.add_argument("--hold", type=int, default=30, help="seconds to hold after ramp")
    args = ap.parse_args()

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, args.conns + 1024), hard))

    stats = Stats()
    stop = asyncio.Event()
    tasks = []
    start = time.monotonic()
    for i in range(args.conns):
        tasks.append(asyncio.create_task(client(i, args, stats, stop)))
        if (i + 1) % args.rate == 0:
            await asyncio.sleep(1)
            print(f"[ramp] launched={i + 1} authed={stats.authed} "
                  f"connect_fail={stats.connect_fail} auth_fail={stats.auth_fail} dropped={stats.dropped}",
                  flush=True)
    print(f"[ramp] done in {time.monotonic() - start:.1f}s", flush=True)

    for t in range(args.hold):
        await asyncio.sleep(1)
        if t % 5 == 4:
            print(f"[hold] t={t + 1}s alive={stats.authed - stats.dropped} pushes={stats.pushes} "
                  f"dropped={stats.dropped}", flush=True)
    stop.set()
    await asyncio.sleep(0.5)
    for t in tasks:
        t.cancel()

    print(json.dumps({
        "target": args.conns,
        "connected": stats.connected,
        "authed": stats.authed,
        "connect_fail": stats.connect_fail,
        "auth_fail": stats.auth_fail,
        "dropped_during_hold": stats.dropped,
        "auth_ms_p50": round(statistics.median(stats.auth_ms), 1) if stats.auth_ms else None,
        "auth_ms_p99": round(pct(stats.auth_ms, 0.99), 1),
        "pushes_received": stats.pushes,
        "push_ms_p50": round(statistics.median(stats.push_ms), 1) if stats.push_ms else None,
        "push_ms_p99": round(pct(stats.push_ms, 0.99), 1),
        "push_ms_max": round(max(stats.push_ms), 1) if stats.push_ms else None,
    }, indent=2), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
