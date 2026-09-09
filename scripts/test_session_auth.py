#!/usr/bin/env python3
import argparse
import json

import requests

from jh_xxtea import decrypt_payload, encrypt_payload


def post(base: str, action: str, payload: dict, version: int) -> requests.Response:
    return requests.post(
        f"{base.rstrip('/')}/{action}?plat=ANDR&ver={version}&channel=none",
        data=encrypt_payload(payload, version),
        timeout=20,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--acc", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--version", type=int, default=478)
    args = parser.parse_args()

    no_session = post(args.base, "downloadSave", {"acc": args.acc, "area": 1}, args.version).json()
    if no_session.get("code") != 401:
        raise RuntimeError(f"missing-session request was not rejected: {no_session}")

    login = post(
        args.base, "login", {"acc": args.acc, "psw": args.password, "style": 2}, args.version
    ).json()
    token = login.get("session", "")
    if login.get("code") != 0 or len(token) != 64:
        raise RuntimeError(f"login did not issue a session: {login}")

    download = post(
        args.base,
        "downloadSave",
        {"acc": "forged-account", "area": 1, "session": token},
        args.version,
    ).json()
    if download.get("code") != 0:
        raise RuntimeError(f"valid session was rejected: {download}")

    init_response = post(
        args.base,
        "getInitData",
        {"acc": "forged-account", "area": 1, "session": token},
        args.version,
    )
    init_data = json.loads(decrypt_payload(init_response.text, args.version))
    if init_data.get("code") != 0:
        raise RuntimeError(f"authenticated getInitData failed: {init_data}")

    print("session_auth=ok missing_rejected=1 token_bound_account=1 init=ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
