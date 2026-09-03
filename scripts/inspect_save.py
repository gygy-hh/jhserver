#!/usr/bin/env python3
"""解密云存档 dat.json，检查触发镇压的字段（bl / gold）。

用法: python scripts/inspect_save.py <存档文件> [save_index]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from jh_xxtea import decrypt_dat, get_dat_cipher, split_blob


def read_blob(path: Path) -> str:
    raw = path.read_bytes()
    if path.suffix == ".sav":
        return raw.decode("utf-8", "replace")
    return json.loads(raw.decode("utf-8"))["save"]


def main() -> None:
    path = Path(sys.argv[1])
    save_index = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    blob = read_blob(path)
    parts = split_blob(blob)
    print("segments:", [p for i, p in enumerate(parts) if i % 2 == 1][:20])

    dat_cipher = get_dat_cipher(blob)
    try:
        mode, plain = decrypt_dat(dat_cipher, save_index)
    except ValueError:
        for idx in range(0, 80):
            try:
                mode, plain = decrypt_dat(dat_cipher, idx)
            except ValueError:
                continue
            print("!! matched save_index:", idx)
            break
        else:
            raise

    doc = json.loads(plain)
    print("mode:", mode)
    print("has bl:", "bl" in doc, "->", doc.get("bl"))
    print("gold:", doc.get("gold"), "(suppression threshold >399999)")
    print("myGift keys:", list(doc.get("myGift", {}).keys())[:5])


if __name__ == "__main__":
    main()
