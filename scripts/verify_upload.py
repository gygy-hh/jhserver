#!/usr/bin/env python3
"""验证线上 uploadSave / downloadSave 链路是否正常。

用测试账号（默认 test_probe_acc）走一次完整流程：
  downloadSave(空) -> uploadSave -> downloadSave(应拿到刚上传的内容)

用法: python scripts/verify_upload.py [acc] [area] [base_url] [ver]
"""

from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from jh_xxtea import encrypt_payload


def post(base: str, ver: int, action: str, payload: dict) -> dict:
    body = encrypt_payload(payload, ver).encode("ascii")
    req = urllib.request.Request(
        f"{base}/{action}?ver={ver}&plat=ANDR&channel=none",
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> None:
    acc = sys.argv[1] if len(sys.argv) > 1 else "test_probe_acc"
    area = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    base = sys.argv[3] if len(sys.argv) > 3 else "http://39.107.52.206:18080"
    ver = int(sys.argv[4]) if len(sys.argv) > 4 else 478

    print(f"[1/3] downloadSave 之前 …")
    before = post(base, ver, "downloadSave", {"acc": acc, "area": area})
    print("      code=%s save_bytes=%s" % (before.get("code"), len(before.get("save") or "")))

    marker = json.dumps({"probe": "disk-write-check", "ts": 1}, separators=(",", ":"))
    print(f"[2/3] uploadSave …")
    up = post(
        base,
        ver,
        "uploadSave",
        {"acc": acc, "area": area, "save": marker, "username": "probe", "lev": 1},
    )
    print("      code=%s msg=%s" % (up.get("code"), up.get("msg")))
    if up.get("code") != 0:
        print("\n=> 上传仍然失败，服务端返回：", json.dumps(up, ensure_ascii=False))
        sys.exit(1)

    print(f"[3/3] downloadSave 之后 …")
    after = post(base, ver, "downloadSave", {"acc": acc, "area": area})
    got = after.get("save") or ""
    print("      code=%s save_bytes=%s" % (after.get("code"), len(got)))

    if got.strip() == marker:
        print("\n=> 链路正常：写入并成功读回。")
    else:
        print("\n=> 上传返回成功，但读回内容不一致：")
        print("     期望:", marker)
        print("     实际:", got[:200])


if __name__ == "__main__":
    main()
