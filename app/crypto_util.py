"""JHCrypto / JhUtility 协议加解密（与 libcocos2dcpp.so 对齐）."""

from __future__ import annotations

import base64
import hashlib
import json
from typing import Any

import xxtea

# 活动校验盐，客户端 getInitData 响应里 mtt 会与此组合做 MD5
MTT_SALT = "17031703"

# 需要加密响应体的接口（getHttpData 第 4 参数为 1）
ENCRYPTED_RESPONSE_ACTIONS = frozenset({"getInitData"})

# POST 体为 Base64(XXTEA(JSON)) 的接口；smsCode 例外为明文 form
ENCRYPTED_BODY_ACTIONS = frozenset(
    {
        "getInitData",
        "login",
        "mail",
        "uploadSave",
        "downloadSave",
        "findSave",
        "reportChong",
        "recvJiHuoMa",
        "selTopFightPower",
        "uploadFightPower",
        "lunJianFightEnd",
        "lunJianFindEnemy",
        "selTopLunJian",
        "selTopWuDao",
        "findEnemy",
        "wuDaoFightEnd",
        "idCard",
    }
)


def md5_hex(data: bytes | str) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.md5(data).hexdigest()


def get_zhiling_psw(app_version: int) -> bytes:
    """JhUtility::getZhiLingPsw — MD5(str(version)) 的前 16 个十六进制字符."""
    digest = md5_hex(str(app_version))
    return digest[:16].encode("ascii")


def encrypt_payload(payload: dict[str, Any] | str, app_version: int) -> str:
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    key = get_zhiling_psw(app_version)
    encrypted = xxtea.encrypt(text.encode("utf-8"), key)
    return base64.b64encode(encrypted).decode("ascii")


def decrypt_payload(cipher_text: str, app_version: int) -> str:
    key = get_zhiling_psw(app_version)
    raw = base64.b64decode(cipher_text)
    plain = xxtea.decrypt(raw, key)
    if plain is None:
        raise ValueError("XXTEA 解密失败，请检查 GAME_VERSION 是否与客户端一致")
    return plain.decode("utf-8")


def parse_request_json(body: str, action: str, app_version: int) -> dict[str, Any]:
    if action not in ENCRYPTED_BODY_ACTIONS:
        return {}
    if not body.strip():
        return {}
    try:
        text = decrypt_payload(body.strip(), app_version)
    except Exception as exc:
        raise ValueError(f"请求体解密失败: {exc}") from exc
    data = json.loads(text)
    if not isinstance(data, dict):
        raise ValueError("解密后的 JSON 必须是对象")
    return data


def build_response_body(action: str, payload: dict[str, Any], app_version: int) -> str:
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    if action in ENCRYPTED_RESPONSE_ACTIONS:
        return encrypt_payload(text, app_version)
    return text


def calc_mtt(data_account: int) -> str:
    return md5_hex(f"{data_account}{MTT_SALT}")
