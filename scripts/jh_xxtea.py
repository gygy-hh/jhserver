#!/usr/bin/env python3
"""XXTEA 与 dat.json 密钥推导，与 src/crypto.cpp、src/save_crypto.cpp 保持一致。

注意：PyPI 的 xxtea 包用 PKCS#7 填充，与客户端/服务端的「长度存末位 uint32」不兼容，
所以这里自己实现。
"""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

DELTA = 0x9E3779B9
MASK = 0xFFFFFFFF

SALT_NEW2 = b"habo6qeegjpqdgfl"
SALT_PAPA = b"bc86tycaxnml5uop"
FIXED_NEW2 = b"vabi98dftdlpzeag"
FIXED_PAPA = b"acdf0899cklnpxzdh"
LEGACY_KEY = b"ab1234abab1234ab"


def _to_u32(data: bytes, include_length: bool) -> list[int]:
    n = (len(data) + 3) // 4 + (1 if include_length else 0)
    v = [0] * n
    for i, b in enumerate(data):
        v[i >> 2] |= b << ((i & 3) * 8)
    if include_length:
        v[n - 1] = len(data)
    return v


def _from_u32(v: list[int], length: int) -> bytes:
    return bytes((v[i >> 2] >> ((i & 3) * 8)) & 0xFF for i in range(length))


def _key_words(key: bytes) -> list[int]:
    return _to_u32(key.ljust(16, b"\0")[:16], False)


def _mx(z: int, y: int, total: int, k: int) -> int:
    return ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((total ^ y) + (k ^ z))) & MASK


def _btea_encrypt(v: list[int], key: list[int]) -> None:
    n = len(v)
    if n <= 1:
        return
    rounds = 6 + 52 // n
    total = 0
    z = v[n - 1]
    for _ in range(rounds):
        total = (total + DELTA) & MASK
        e = (total >> 2) & 3
        for p in range(n - 1):
            y = v[p + 1]
            v[p] = z = (v[p] + _mx(z, y, total, key[(p & 3) ^ e])) & MASK
        y = v[0]
        v[n - 1] = z = (v[n - 1] + _mx(z, y, total, key[((n - 1) & 3) ^ e])) & MASK


def _btea_decrypt(v: list[int], key: list[int]) -> None:
    n = len(v)
    if n <= 1:
        return
    rounds = 6 + 52 // n
    total = (rounds * DELTA) & MASK
    y = v[0]
    for _ in range(rounds):
        e = (total >> 2) & 3
        for p in range(n - 1, 0, -1):
            z = v[p - 1]
            v[p] = y = (v[p] - _mx(z, y, total, key[(p & 3) ^ e])) & MASK
        z = v[n - 1]
        v[0] = y = (v[0] - _mx(z, y, total, key[e])) & MASK
        total = (total - DELTA) & MASK


def xxtea_encrypt(plain: bytes, key: bytes) -> bytes:
    if not plain:
        return b""
    v = _to_u32(plain, True)
    _btea_encrypt(v, _key_words(key))
    return b"".join(word.to_bytes(4, "little") for word in v)


def xxtea_decrypt(raw: bytes, key: bytes) -> bytes:
    if not raw or len(raw) % 4 != 0:
        raise ValueError("invalid cipher length")
    v = [int.from_bytes(raw[i : i + 4], "little") for i in range(0, len(raw), 4)]
    _btea_decrypt(v, _key_words(key))
    length = v[-1]
    if length > (len(v) - 1) * 4:
        raise ValueError("invalid plain length")
    return _from_u32(v, length)


def md5_hex(data: bytes | str) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.md5(data).hexdigest()


def payload_key(app_version: int) -> bytes:
    """JhUtility::getZhiLingPsw — 请求/响应体密钥。"""
    return md5_hex(str(app_version))[:16].encode("ascii")


def encrypt_payload(payload: dict | str, app_version: int) -> str:
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    return base64.b64encode(xxtea_encrypt(text.encode("utf-8"), payload_key(app_version))).decode("ascii")


def decrypt_payload(cipher_b64: str, app_version: int) -> str:
    raw = base64.b64decode(cipher_b64)
    return xxtea_decrypt(raw, payload_key(app_version)).decode("utf-8", "replace")


def _interleave(fill: int, fixed: bytes) -> bytes:
    out = bytearray()
    for ch in fixed:
        out.append(ch)
        out.append(fill)
    return bytes(out)


def derive_new2(save_index: int) -> bytes:
    md5 = md5_hex(_interleave(SALT_NEW2[save_index % 16], FIXED_NEW2))
    off = save_index % 18
    return md5[off : off + 16].encode("ascii")


def derive_papa(save_index: int) -> bytes:
    md5 = md5_hex(_interleave(SALT_PAPA[save_index % 16], FIXED_PAPA))
    off = save_index % 7
    return md5[off : off + 16].encode("ascii")


def override_key(save_index: int) -> bytes | None:
    path = Path(__file__).resolve().parent.parent / "data" / "save_keys.json"
    if not path.exists():
        return None
    val = json.loads(path.read_text(encoding="utf-8")).get("by_index", {}).get(str(save_index))
    return val[:16].encode("ascii") if val else None


def _key_for_mode(mode: str, save_index: int) -> bytes:
    if mode == "override":
        key = override_key(save_index)
        if key is None:
            raise ValueError("override key missing")
        return key
    if mode == "new2":
        return derive_new2(save_index)
    if mode == "papa":
        return derive_papa(save_index)
    if mode == "legacy":
        return LEGACY_KEY
    raise ValueError(f"unknown dat key mode: {mode}")


def decrypt_dat(cipher_b64: str, save_index: int) -> tuple[str, str]:
    """返回 (使用的密钥模式, dat.json 明文)。"""
    raw = base64.b64decode(cipher_b64)
    candidates: list[tuple[str, bytes]] = []
    if (ov := override_key(save_index)) is not None:
        candidates.append(("override", ov))
    candidates += [
        ("new2", derive_new2(save_index)),
        ("papa", derive_papa(save_index)),
        ("legacy", LEGACY_KEY),
    ]
    for name, key in candidates:
        try:
            plain = xxtea_decrypt(raw, key)
        except ValueError:
            continue
        if plain[:1] in (b"{", b"["):
            return name, plain.decode("utf-8", "replace")
    raise ValueError("dat.json decrypt failed")


def encrypt_dat(plain: str, save_index: int, mode: str) -> str:
    """按指定模式加密 dat.json 明文，返回 base64 密文。"""
    raw = xxtea_encrypt(plain.encode("utf-8"), _key_for_mode(mode, save_index))
    return base64.b64encode(raw).decode("ascii")


def split_blob(blob: str) -> list[str]:
    return blob.split("____")


def get_dat_cipher(blob: str) -> str:
    parts = split_blob(blob)
    return parts[parts.index("dat.json") + 1]


def set_dat_cipher(blob: str, cipher_b64: str) -> str:
    parts = split_blob(blob)
    idx = parts.index("dat.json")
    parts[idx + 1] = cipher_b64
    return "____".join(parts)


def apply_bl(blob: str, save_index: int) -> tuple[str, bool]:
    """写入 dat.json 的 bl=1。返回 (新 blob, 是否有改动)。"""
    mode, plain = decrypt_dat(get_dat_cipher(blob), save_index)
    doc = json.loads(plain)
    if not isinstance(doc, dict):
        raise ValueError("dat.json is not an object")
    if "bl" in doc:
        return blob, False
    doc["bl"] = 1
    # 保持原有键序，仅追加 bl
    cipher = encrypt_dat(json.dumps(doc, ensure_ascii=False, separators=(",", ":")), save_index, mode)
    return set_dat_cipher(blob, cipher), True
