import base64
import hashlib
import json
import random
import struct
import sys
import urllib.request

DELTA = 0x9E3779B9
VER = 520


def to_u32(data, include_len):
    n = (len(data) + 3) // 4
    padded = data + b"\0" * (n * 4 - len(data))
    v = list(struct.unpack("<%dI" % n, padded)) if n else []
    if include_len:
        v.append(len(data))
    return v


def btea(v, k):
    n = len(v)
    rounds = 6 + 52 // n
    s = 0
    z = v[n - 1]
    while rounds:
        s = (s + DELTA) & 0xFFFFFFFF
        e = (s >> 2) & 3
        for p in range(n):
            y = v[(p + 1) % n]
            mx = ((((z >> 5) ^ (y << 2) & 0xFFFFFFFF) + ((y >> 3) ^ (z << 4) & 0xFFFFFFFF)) & 0xFFFFFFFF) ^ \
                 (((s ^ y) + (k[(p & 3) ^ e] ^ z)) & 0xFFFFFFFF)
            v[p] = (v[p] + mx) & 0xFFFFFFFF
            z = v[p]
        rounds -= 1
    return v


def encrypt(plain):
    key = hashlib.md5(str(VER).encode()).hexdigest()[:16].encode()
    v = btea(to_u32(plain.encode(), True), to_u32(key, False))
    return base64.b64encode(struct.pack("<%dI" % len(v), *v)).decode()


def main():
    acc = sys.argv[1] if len(sys.argv) > 1 else "199%08d" % random.randint(0, 99999999)
    psw = sys.argv[2] if len(sys.argv) > 2 else "abc123"
    body = encrypt(json.dumps({"acc": acc, "psw": psw}, separators=(",", ":")))
    url = "http://39.107.52.206:18080/login?plat=ANDR&ver=%d&channel=xuanwu_3" % VER
    req = urllib.request.Request(url, data=body.encode(), method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        print(acc, resp.status, resp.read().decode())


if __name__ == "__main__":
    main()
