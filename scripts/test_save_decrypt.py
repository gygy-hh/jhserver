#!/usr/bin/env python3
"""离线验证 dat.json 加解密（算法与 libcocos2dcpp.so 一致）。"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import xxtea

SALT_NEW2 = b"habo6qeegjpqdgfl"
SALT_PAPA = b"bc86tycaxnml5uop"
FIXED_NEW2 = b"vabi98dftdlpzeag"
FIXED_PAPA = b"acdf0899cklnpxzdh"
LEGACY = b"ab1234abab1234ab"


def interleave(fill: int, fixed: bytes) -> bytes:
    out = bytearray()
    for ch in fixed:
        out.append(ch)
        out.append(fill)
    return bytes(out)


def derive_new2(save_index: int) -> bytes:
    fill = SALT_NEW2[save_index % 16]
    material = interleave(fill, FIXED_NEW2)
    md5 = hashlib.md5(material).hexdigest()
    off = save_index % 18
    return md5[off : off + 16].encode("ascii")


def derive_papa(save_index: int) -> bytes:
    fill = SALT_PAPA[save_index % 16]
    material = interleave(fill, FIXED_PAPA)
    md5 = hashlib.md5(material).hexdigest()
    off = save_index % 7
    return md5[off : off + 16].encode("ascii")


def try_decrypt(cipher_b64: str, save_index: int) -> tuple[str, str]:
    import base64

    raw = base64.b64decode(cipher_b64)
    for name, key in (
        ("new2", derive_new2(save_index)),
        ("papa", derive_papa(save_index)),
        ("legacy", LEGACY),
    ):
        plain = xxtea.decrypt(raw, key)
        if plain and plain[:1] in (b"{", b"["):
            return name, plain.decode("utf-8", "replace")
    raise ValueError("decrypt failed")


def main() -> None:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "data/saves/19848015669_1.json")
    save_index = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    data = json.loads(path.read_text(encoding="utf-8"))
    blob = data["save"]
    parts = blob.split("____")
    dat_cipher = parts[parts.index("dat.json") + 1]
    mode, plain = try_decrypt(dat_cipher, save_index)
    print("mode:", mode)
    print("plain head:", plain[:200])
    doc = json.loads(plain)
    print("myGift keys:", list(doc.get("myGift", {}).keys())[:5])


if __name__ == "__main__":
    main()
