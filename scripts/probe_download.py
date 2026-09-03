#!/usr/bin/env python3
"""向线上服务器发一次 downloadSave，检查返回存档的 dat.json 是否已剥离 bl。

用法: python scripts/probe_download.py <acc> [area] [base_url] [ver]
"""

from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from jh_xxtea import decrypt_dat, encrypt_payload, get_dat_cipher


def main() -> None:
    acc = sys.argv[1]
    area = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    base = sys.argv[3] if len(sys.argv) > 3 else "http://8.219.65.240:18080"
    ver = int(sys.argv[4]) if len(sys.argv) > 4 else 478

    body = encrypt_payload({"acc": acc, "area": area}, ver).encode("ascii")
    req = urllib.request.Request(
        f"{base}/downloadSave?ver={ver}&plat=ANDR&channel=none",
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        payload = json.loads(resp.read().decode("utf-8"))

    print("code:", payload.get("code"), "msg:", payload.get("msg"))
    blob = payload.get("save") or payload.get("data", {}).get("save", "")
    print("save bytes:", len(blob))
    if not blob:
        return

    mode, plain = decrypt_dat(get_dat_cipher(blob), area)
    doc = json.loads(plain)
    print("mode:", mode)
    print("has bl:", "bl" in doc, "->", doc.get("bl"))
    print("gold:", doc.get("gold"))


if __name__ == "__main__":
    main()
