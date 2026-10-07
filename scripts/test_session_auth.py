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

def response_json(response: requests.Response, action: str, version: int) -> dict:
    if action == "getInitData":
        return json.loads(decrypt_payload(response.text, version))
    return response.json()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--acc", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--version", type=int, default=478)
    args = parser.parse_args()

    no_session_response = post(
        args.base, "getInitData", {"acc": args.acc, "area": 1}, args.version
    )
    no_session = response_json(no_session_response, "getInitData", args.version)
    if no_session.get("code") != 401:
        raise RuntimeError(f"missing-session request was not rejected: {no_session}")

    login = post(
        args.base, "login", {"acc": args.acc, "psw": args.password, "style": 2}, args.version
    ).json()
    token = login.get("session", "")
    if login.get("code") != 0 or len(token) != 64 or login.get("acc") != args.acc:
        raise RuntimeError(f"login did not issue a session: {login}")

    mismatch_response = post(
        args.base,
        "getInitData",
        {"acc": "forged-account", "area": 1, "session": token},
        args.version,
    )
    mismatch = response_json(mismatch_response, "getInitData", args.version)
    if mismatch.get("code") != 401 or mismatch.get("msg") != "session account mismatch":
        raise RuntimeError(f"cross-account session was not rejected: {mismatch}")

    for area in (1, 66):
        init_response = post(
            args.base,
            "getInitData",
            {"acc": args.acc, "area": area, "session": token},
            args.version,
        )
        init_data = response_json(init_response, "getInitData", args.version)
        if init_data.get("code") != 0 or init_data.get("area") != area:
            raise RuntimeError(f"authenticated getInitData failed area={area}: {init_data}")

    invalid_area_response = post(
        args.base,
        "getInitData",
        {"acc": args.acc, "area": 67, "session": token},
        args.version,
    )
    invalid_area = response_json(invalid_area_response, "getInitData", args.version)
    if invalid_area.get("code") == 0:
        raise RuntimeError(f"invalid area was accepted: {invalid_area}")

    print("session_auth=ok missing_rejected=1 mismatch_rejected=1 areas=1,66 invalid=67")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
