import json
import logging
import os
import platform
import secrets
import socket
import sys
import threading
import traceback
from logging.handlers import RotatingFileHandler
import uuid
import asyncio
import hashlib
import sqlite3
import time
import queue
from concurrent.futures import ThreadPoolExecutor
import urllib.error
import urllib.request
from urllib.parse import urlsplit
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from mitmproxy import ctx, http
from mitmproxy.utils import asyncio_utils


ADDON_BUILD = "2026-10-01.game-server-heartbeat.1"
TARGET_HOST = "limep2-pub.global.plaync.com"
TARGET_PATH = "/stomp"
CHAT_API_HOST = "lime-p2-api.global.plaync.com"
CHAT_API_PREFIX = "/gameClient/"
OUTPUT_FILE = Path("chat_messages.jsonl")
SEND_WHISPER_URL = "https://lime-arizona-p4-api.plaync.com/gameClient/sendWhisper"
LIME_HEADER_NAMES = {
    "lime-api-version": "Lime-API-Version",
    "lime-trace-id": "Lime-Trace-Id",
    "lime-app-version": "Lime-App-Version",
    "lime-sdk-version": "Lime-Sdk-Version",
    "lime-device-name": "Lime-Device-Name",
}

def _load_mqtt_configuration():
    directory = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else Path(__file__).resolve().parent
    filename = Path(os.getenv('AION2_MQTT_CONFIG', str(directory / 'mqtt.private.local.json')))
    if not filename.is_file():
        return {}
    value = json.loads(filename.read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError('MQTT configuration must be a JSON object')
    return value


_mqtt_config = _load_mqtt_configuration()
for _key, _env in [('host', 'AION2_MQTT_HOST'), ('port', 'AION2_MQTT_PORT'), ('room', 'AION2_RELAY_ROOM'), ('prefix', 'AION2_SIGNAL_PREFIX')]:
    if _key in _mqtt_config:
        os.environ.setdefault(_env, str(_mqtt_config[_key]))
MQTT_USERNAME = os.getenv('AION2_MQTT_USERNAME') or _mqtt_config.get('username', '') or 'aion2-private'
MQTT_PASSWORD = os.getenv('AION2_MQTT_PASSWORD') or _mqtt_config.get('password', '')

MQTT_PREFIX = os.getenv("AION2_SIGNAL_PREFIX", "aion2-chat-bridge")
MQTT_ROOM = os.getenv("AION2_RELAY_ROOM", "aion2-local")
AGENT_ID = os.getenv(
    "AION2_AGENT_ID",
    # Keep the default identity stable across restarts so its durable queue reopens.
    f"{platform.node() or socket.gethostname()}-" + hashlib.sha256(
        str(Path(globals().get('__file__', os.getcwd())).resolve()).casefold().encode()
    ).hexdigest()[:12],
)
MQTT_HOST = os.getenv("AION2_MQTT_HOST", "od43e177.ala.cn-shenzhen.emqxsl.cn")
MQTT_PORT = int(os.getenv("AION2_MQTT_PORT", "8883"))

def _decode_message(content: bytes) -> str:
    return content.decode("utf-8", errors="replace")


def _parse_stomp_frames(raw: str) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []

    for chunk in raw.split("\x00"):
        chunk = chunk.lstrip("\r\n")
        if not chunk.strip():
            continue

        header_part, separator, body = chunk.partition("\n\n")
        if not separator:
            header_part, separator, body = chunk.partition("\r\n\r\n")
        if not separator:
            continue

        lines = header_part.replace("\r\n", "\n").split("\n")
        command = lines[0].strip()
        headers: dict[str, str] = {}

        for line in lines[1:]:
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            headers[key.strip()] = value.strip()

        frames.append(
            {
                "command": command,
                "headers": headers,
                "body": body,
            }
        )

    return frames


def _try_json(value: Any) -> Any:
    if not isinstance(value, str):
        return value

    stripped = value.strip()
    if not stripped or stripped[0] not in "[{":
        return value

    try:
        return json.loads(stripped)
    except json.JSONDecodeError:
        return value


def _expand_nested_json(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: _expand_nested_json(_try_json(item)) for key, item in value.items()}
    if isinstance(value, list):
        return [_expand_nested_json(item) for item in value]
    return value


def _find_first(data: Any, keys: set[str]) -> Any:
    if isinstance(data, dict):
        for key, value in data.items():
            if key.lower() in keys and value not in (None, ""):
                return value
        for value in data.values():
            found = _find_first(value, keys)
            if found not in (None, ""):
                return found
    elif isinstance(data, list):
        for value in data:
            found = _find_first(value, keys)
            if found not in (None, ""):
                return found
    return None


def _summarize_chat(payload: Any) -> str:
    meta = _extract_chat_meta(payload)
    sender = meta.get("sender") or "unknown"
    content = meta.get("content")
    receiver = meta.get("receiver")

    if content and meta.get("kind") == "private" and receiver:
        return f"[私聊] {sender} -> {receiver}: {content}"
    if content:
        return f"{sender}: {content}"

    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _extract_chat_meta(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        return {
            "kind": "unknown",
            "roomType": None,
            "sender": None,
            "receiver": None,
            "content": None,
            "roomKey": None,
        }

    data = payload.get("jsonData")
    if not isinstance(data, dict):
        data = {}

    room_info = data.get("gameRoomKeyInfo")
    if not isinstance(room_info, dict):
        room_info = {}

    room_type = room_info.get("type")
    receiver = data.get("receiverUserName")
    kind = "private" if room_type == "ONE_ON_ONE" or receiver else "room"

    return {
        "kind": kind,
        "roomType": room_type,
        "sender": data.get("userName") or data.get("alias"),
        "senderCharacterId": data.get("playNcCharId"),
        "receiver": receiver,
        "receiverCharacterId": data.get("receiverCharacterId"),
        "receiverGameUserId": data.get("receiverGameUserId"),
        "content": data.get("content"),
        "initialContent": data.get("initialContent"),
        "serverId": data.get("serverId"),
        "receiverServerId": data.get("receiverServerId"),
        "roomId": data.get("roomId"),
        "roomKey": room_info.get("roomKey"),
    }


def _extract_request_chat_meta(body: Any, url: str) -> dict[str, Any]:
    if not isinstance(body, dict):
        return {
            "kind": "unknown",
            "roomType": None,
            "sender": None,
            "receiver": None,
            "content": None,
            "roomKey": None,
        }

    message_info = body.get("gameMessageInfo")
    if not isinstance(message_info, dict):
        message_info = {}

    game_user_key = body.get("gameUserKey")
    if not isinstance(game_user_key, dict):
        game_user_key = {}

    is_private = "sendWhisper" in url
    return {
        "kind": "private" if is_private else "outgoing",
        "roomType": "ONE_ON_ONE" if is_private else "OUTGOING",
        "sender": message_info.get("userName"),
        "receiver": None,
        "senderCharacterId": None,
        "receiverCharacterId": game_user_key.get("characterId"),
        "receiverGameUserId": None,
        "content": message_info.get("content"),
        "initialContent": message_info.get("content"),
        "serverId": game_user_key.get("serverKey"),
        "receiverServerId": game_user_key.get("serverKey"),
        "roomId": None,
        "roomKey": None,
    }


def _fallback_summarize_chat(payload: Any) -> str:
    sender = _find_first(
        payload,
        {
            "sender",
            "sendername",
            "username",
            "nickname",
            "charactername",
            "from",
            "name",
        },
    )
    text = _find_first(
        payload,
        {
            "message",
            "msg",
            "text",
            "content",
            "body",
            "chat",
            "chatmessage",
        },
    )

    if sender and text:
        return f"{sender}: {text}"
    if text:
        return str(text)
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _json_body(flow: http.HTTPFlow, response: bool = False) -> Any:
    content = (
        flow.response.raw_content
        if response and flow.response is not None
        else flow.request.raw_content
    )
    if not content:
        return None
    try:
        return json.loads(content.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return None


def _auth_header(value: str | None) -> str | None:
    if not value:
        return None
    value = value.strip()
    if not value:
        return None
    return value if value.lower().startswith("bearer ") else f"Bearer {value}"


def _redact(value: Any) -> Any:
    if value in (None, ""):
        return value
    return "<REDACTED>"


def _display_header_value(name: str, value: str) -> str:
    if name.lower() != "authorization":
        return value
    if os.getenv("AION2_PRINT_FULL_AUTH", "").strip().lower() in {"1", "true", "yes", "on"}:
        return value
    if value.lower().startswith("bearer ") and len(value) > 32:
        token = value[7:]
        return f"Bearer {token[:16]}...{token[-8:]}"
    return "<REDACTED>"


def _json_string(value: Any) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _print_raw_whisper_request(body: dict[str, Any], headers: dict[str, str], body_bytes: bytes) -> None:
    ordered_names = [
        "Host",
        "Accept",
        "Content-Type",
        "Authorization",
        "Lime-API-Version",
        "Lime-Trace-Id",
        "Lime-App-Version",
        "Lime-Sdk-Version",
        "Lime-Device-Name",
        "User-Agent",
        "Content-Length",
    ]
    display_headers = {
        "Host": "lime-p2-api.global.plaync.com",
        **headers,
        "Content-Length": str(len(body_bytes)),
    }
    body_text = json.dumps(body, ensure_ascii=False, indent=2)
    lines = [
        "[WHISPER RAW REQUEST BEGIN]",
        f"POST {SEND_WHISPER_URL} HTTP/1.1",
    ]
    for name in ordered_names:
        value = display_headers.get(name)
        if value not in (None, ""):
            lines.append(f"{name}: {_display_header_value(name, str(value))}")
    lines.extend(["", "JSON", body_text, "[WHISPER RAW REQUEST END]"])
    print("\n".join(lines), flush=True)


def _debug_whisper_request(body: dict[str, Any], headers: dict[str, str]) -> dict[str, Any]:
    message_info = body.get("gameMessageInfo") or {}
    optional = message_info.get("optional")
    return {
        "url": SEND_WHISPER_URL,
        "headers": {
            key: value for key, value in headers.items() if key.lower().startswith("lime-")
        },
        "gameUserKey": body.get("gameUserKey"),
        "gameMessageInfo": {
            "keys": sorted(message_info.keys()),
            "userName": message_info.get("userName"),
            "type": message_info.get("type"),
            "subType": message_info.get("subType"),
            "content": message_info.get("content"),
            "optionalType": type(optional).__name__,
            "optionalPreview": optional[:160] if isinstance(optional, str) else optional,
        },
        "topLevelKeys": sorted(body.keys()),
    }


def _normalize_room_key_info(value: Any) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    room_type = value.get("type")
    server_key = value.get("serverKey")
    room_key = value.get("roomKey")
    if not room_type or not server_key or not room_key:
        return None
    return {
        "type": str(room_type),
        "serverKey": str(server_key),
        "roomKey": str(room_key),
    }


def _is_plaync_host(host: str | None) -> bool:
    return "plaync.com" in (host or "").lower()


def _flow_plaync_host(flow: Any) -> str | None:
    """Return the TLS/HTTP PlayNC host even when an accelerator exposes only an IP CONNECT target."""
    request = getattr(flow, "request", None)
    headers = getattr(request, "headers", {}) if request is not None else {}
    candidates = [
        getattr(request, "host", None),
        getattr(request, "pretty_host", None),
        headers.get("host") if hasattr(headers, "get") else None,
        getattr(getattr(flow, "client_conn", None), "sni", None),
        getattr(getattr(flow, "server_conn", None), "sni", None),
    ]
    for candidate in candidates:
        if not candidate:
            continue
        host = str(candidate).strip().lower().split(":", 1)[0]
        if _is_plaync_host(host):
            return host
    return None


def _effective_flow_url(flow: Any) -> str:
    url = flow.request.pretty_url
    if _is_plaync_host(urlsplit(url).hostname):
        return url
    host = _flow_plaync_host(flow)
    if not host:
        return url
    path = getattr(flow.request, "path", None) or "/"
    return f"https://{host}{path}"


def _extract_character_object_key(url: str) -> tuple[str | None, str | None]:
    marker = "/objects/character."
    if marker not in url:
        return None, None
    tail = url.split(marker, 1)[1].split("?", 1)[0].split("/", 1)[0]
    parts = tail.split(".")
    if len(parts) < 2:
        return None, None
    server_key, character_id = parts[0], parts[1]
    if not server_key or not character_id:
        return None, None
    return server_key, character_id


def _client_info(flow_or_context: Any) -> str:
    client = getattr(flow_or_context, "client_conn", None)
    if client is None:
        client = getattr(flow_or_context, "client", None)
    if client is None:
        return "client=?"

    peername = getattr(client, "peername", None)
    sni = getattr(client, "sni", None)
    tls_established = getattr(client, "tls_established", None)
    proxy_mode = getattr(client, "proxy_mode", None)
    return (
        f"client={peername} mode={proxy_mode} "
        f"sni={sni} tls_established={tls_established}"
    )


def _profile_name(profile: dict[str, Any]) -> str | None:
    for key in ("userName", "characterName", "alias"):
        value = profile.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return None


def _normalize_optional(value: Any) -> str | None:
    """Keep the original JSON string; reject empty, malformed or non-object metadata."""
    if isinstance(value, str):
        if not value.strip():
            return None
        try:
            parsed = json.loads(value)
        except (ValueError, TypeError):
            return None
        return value if isinstance(parsed, dict) and parsed else None
    if isinstance(value, dict) and value:
        return json.dumps(value, ensure_ascii=False, allow_nan=False)
    return None


def _chat_api_origin(url):
    parsed = urlsplit(url)
    if parsed.scheme == 'https' and parsed.netloc in (
        'lime-p2-api.global.plaync.com', 'lime-arizona-p4-api.plaync.com'
    ):
        return 'https://' + parsed.netloc
    return None


class RuntimeState:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.data: dict[str, Any] = {
            "authorization": None,
            "authnToken": None,
            "characterId": None,
            "gameUserId": None,
            "serverKey": None,
            "userName": None,
            "gameCode": None,
            "deviceId": None,
            "locale": None,
            "optional": None,
            "userAgent": None,
            "apiHeaders": {},
            "messageInfoTemplate": {},
            "validationInfo": {},
            "extraData": {},
            "subscriptionInfo": {},
            "knownUsers": {},
            "lastUpdated": None,
        }

    def snapshot(self, redact: bool = True) -> dict[str, Any]:
        with self.lock:
            copied = json.loads(json.dumps(self.data, ensure_ascii=False, default=str))
        if redact:
            copied["authorization"] = _redact(copied.get("authorization"))
            copied["authnToken"] = _redact(copied.get("authnToken"))
            sub = copied.get("subscriptionInfo")
            if isinstance(sub, dict):
                sub["passcode"] = _redact(sub.get("passcode"))
        return copied

    def client_identity(self) -> dict[str, Any]:
        with self.lock:
            return {
                "serverId": self.data.get("serverKey"),
                "characterId": self.data.get("characterId"),
                "characterName": self.data.get("userName"),
            }

    def update(self, **values: Any) -> None:
        changed = False
        with self.lock:
            identity_changed = any(
                values.get(key) not in (None, "")
                and str(values[key]) != str(self.data.get(key))
                for key in ("characterId", "serverKey")
            )
            if identity_changed:
                self.data["chatApiOrigin"] = None
                self.data["userName"] = None
                self.data["optional"] = None
                self.data["messageInfoTemplate"] = {}
                self.data["validationInfo"] = {}
                self.data["extraData"] = {}
            if "optional" in values:
                values["optional"] = _normalize_optional(values["optional"])
            for key, value in values.items():
                if value not in (None, ""):
                    self.data[key] = value
                    changed = True
            if not self.data.get("userName"):
                known = self.data["knownUsers"].get(str(self.data.get("characterId")), {})
                if (self.data.get("serverKey") not in (None, "")
                        and str(known.get("serverKey")) == str(self.data["serverKey"])):
                    self.data["userName"] = _profile_name(known)
            if changed:
                self.data["lastUpdated"] = datetime.now(timezone.utc).isoformat()

    def merge_subscription(self, value: Any) -> None:
        if not isinstance(value, dict):
            return
        with self.lock:
            self.data["subscriptionInfo"].update(value)
            self.data["lastUpdated"] = datetime.now(timezone.utc).isoformat()

    def observe_api_headers(self, headers: Any) -> None:
        learned: dict[str, str] = {}
        try:
            items = headers.items(multi=True)
        except TypeError:
            items = headers.items()
        for raw_name, raw_value in items:
            canonical = LIME_HEADER_NAMES.get(str(raw_name).lower())
            if canonical and raw_value not in (None, ""):
                learned[canonical] = str(raw_value)
        if not learned:
            return
        with self.lock:
            self.data["apiHeaders"].update(learned)
            self.data["lastUpdated"] = datetime.now(timezone.utc).isoformat()
        print(
            "[STATE] learned headers "
            + ", ".join(f"{name}={value}" for name, value in learned.items()),
            flush=True,
        )

    def remember_message_template(self, body: Any) -> None:
        if not isinstance(body, dict):
            return
        message_info = body.get("gameMessageInfo")
        if not isinstance(message_info, dict):
            return

        template = {
            key: value
            for key, value in message_info.items()
            if key not in {"content", "initialContent"}
        }
        with self.lock:
            self.data["messageInfoTemplate"].update(template)
            for key in ("validationInfo", "extraData"):
                value = body.get(key)
                if isinstance(value, dict):
                    self.data[key] = value
            self.data["lastUpdated"] = datetime.now(timezone.utc).isoformat()

    def remember_user(
        self,
        character_id: Any,
        user_name: Any = None,
        server_id: Any = None,
        game_user_id: Any = None,
        optional: Any = None,
    ) -> None:
        if character_id in (None, "", "null"):
            return
        key = str(character_id)
        with self.lock:
            users = self.data["knownUsers"]
            item = users.setdefault(key, {})
            if (server_id not in (None, "") and item.get("serverKey") not in (None, "")
                    and str(server_id) != str(item["serverKey"])):
                item.clear()
            item["characterId"] = key
            if user_name not in (None, ""):
                item["userName"] = user_name
            if server_id not in (None, ""):
                item["serverKey"] = str(server_id)
            if game_user_id not in (None, "", "null"):
                item["gameUserId"] = str(game_user_id)
            normalized_optional = _normalize_optional(optional)
            if normalized_optional:
                item["optional"] = normalized_optional
            item["lastSeen"] = datetime.now(timezone.utc).isoformat()
            self.data["lastUpdated"] = item["lastSeen"]
            if (key == str(self.data.get("characterId"))
                    and server_id not in (None, "")
                    and str(server_id) == str(self.data.get("serverKey"))):
                name = _profile_name({"userName": user_name})
                if name:
                    self.data["userName"] = name

    def observe_request(self, flow: http.HTTPFlow) -> None:
        body = _json_body(flow)
        url = _effective_flow_url(flow)
        auth = _auth_header(flow.request.headers.get("authorization"))
        user_agent = flow.request.headers.get("user-agent")
        object_server_key, object_character_id = _extract_character_object_key(url)

        if object_character_id:
            self.remember_user(object_character_id, server_id=object_server_key)

        if _chat_api_origin(url) and urlsplit(url).path == "/gameLogin/loginWithToken":
            if isinstance(body, dict):
                self.update(
                    authnToken=body.get("authnToken"),
                    characterId=body.get("characterId"),
                    gameUserId=body.get("gameUserId"),
                    serverKey=body.get("serverKey"),
                    gameCode=body.get("gameCode"),
                    deviceId=body.get("deviceId"),
                    locale=body.get("locale"),
                    userName=_profile_name(body),
                    optional=body.get("optional"),
                    userAgent=user_agent,
                )
            return

        if not _chat_api_origin(url) or not urlsplit(url).path.startswith("/gameClient/"):
            return

        self.observe_api_headers(flow.request.headers)
        self.update(authorization=auth, userAgent=user_agent, chatApiOrigin=_chat_api_origin(url))
        if not isinstance(body, dict):
            return

        if any(name in url for name in ("sendMessage", "sendWhisper")):
            self.remember_message_template(body)

        message_info = body.get("gameMessageInfo")
        if isinstance(message_info, dict):
            self.update(
                userName=_profile_name(message_info),
                optional=message_info.get("optional"),
                gameCode=message_info.get("gameCode"),
            )

        if "sendWhisper" in url:
            game_user_key = body.get("gameUserKey")
            if isinstance(game_user_key, dict):
                self.remember_user(
                    game_user_key.get("characterId"),
                    server_id=game_user_key.get("serverKey"),
                )

    def observe_response(self, flow: http.HTTPFlow) -> None:
        url = _effective_flow_url(flow)
        body = _json_body(flow, response=True)
        object_server_key, object_character_id = _extract_character_object_key(url)

        if object_character_id:
            self.remember_user(object_character_id, server_id=object_server_key)
            if isinstance(body, dict):
                self.remember_user(
                    object_character_id,
                    user_name=_find_first(body, {"username", "alias", "charactername"}),
                    server_id=object_server_key,
                )

        if _chat_api_origin(url) and urlsplit(url).path == "/gameLogin/loginWithToken":
            self.update(
                authorization=_auth_header(flow.response.headers.get("authorization"))
                if flow.response
                else None
            )
            if isinstance(body, dict):
                self.update(
                    characterId=body.get("characterId"),
                    gameUserId=body.get("gameUserId"),
                    serverKey=body.get("serverKey"),
                    gameCode=body.get("gameCode"),
                    locale=body.get("locale"),
                    userName=_profile_name(body),
                    optional=body.get("optional"),
                )
                self.merge_subscription(body.get("subscriptionInfo"))

    def observe_chat(
        self,
        payload: Any,
        meta: dict[str, Any],
        direction: str | None = None,
    ) -> None:
        data = payload.get("jsonData") if isinstance(payload, dict) else None
        self.remember_user(
            meta.get("senderCharacterId"),
            user_name=meta.get("sender"),
            server_id=meta.get("serverId"),
            optional=data.get("optional") if isinstance(data, dict) else None,
        )
        self.remember_user(
            meta.get("receiverCharacterId"),
            user_name=meta.get("receiver"),
            server_id=meta.get("receiverServerId") or meta.get("serverId"),
            game_user_id=meta.get("receiverGameUserId"),
        )
        if isinstance(payload, dict):
            data = payload.get("jsonData")
            if isinstance(data, dict):
                with self.lock:
                    current_character_id = self.data.get("characterId")
                    current_game_user_id = self.data.get("gameUserId")
                    current_user_name = self.data.get("userName")
                sender_character_id = data.get("playNcCharId")
                sender_game_user_id = data.get("gameUserId")
                is_current_character = (
                    current_character_id
                    and sender_character_id
                    and str(sender_character_id) == str(current_character_id)
                )
                is_current_game_user = (
                    current_game_user_id
                    and sender_game_user_id
                    and str(sender_game_user_id) == str(current_game_user_id)
                )
                sender_server = data.get("serverId") or data.get("serverKey")
                with self.lock:
                    current_server = self.data.get("serverKey")
                same_server = (sender_server not in (None, "") and current_server not in (None, "")
                               and str(sender_server) == str(current_server))
                # An incoming stranger must never become the sender of console whispers.
                same_character = is_current_character or (is_current_game_user and
                    (not sender_character_id or not current_character_id))
                should_learn_self = same_server and same_character
                if should_learn_self:
                    self.apply_self_profile(data)

    def apply_self_profile(self, profile: Any) -> None:
        if not isinstance(profile, dict):
            return
        self.update(
            characterId=profile.get("characterId") or profile.get("playNcCharId"),
            gameUserId=profile.get("gameUserId"),
            serverKey=profile.get("serverKey") or profile.get("serverId"),
            userName=_profile_name(profile),
            gameCode=profile.get("gameCode"),
            locale=profile.get("locale"),
            optional=profile.get("optional"),
        )

    def build_whisper_body(
        self,
        character_id: str,
        server_key: str | None,
        content: str,
        room_key_info: Any = None,
        receiver_game_user_id: Any = None,
    ) -> dict[str, Any]:
        with self.lock:
            user_name = self.data.get("userName") or ""
            optional = _normalize_optional(self.data.get("optional"))
            message_template = dict(self.data.get("messageInfoTemplate") or {})
            validation_info = dict(self.data.get("validationInfo") or {})
            extra_data = dict(self.data.get("extraData") or {})
            known = self.data.get("knownUsers", {}).get(character_id, {})
            server_key = server_key or known.get("serverKey") or self.data.get("serverKey")

        if not server_key:
            raise ValueError("缺少 serverKey：请填写目标服务器")

        if not optional:
            raise ValueError("缺少当前登录角色的有效 optional，未发送。请先在游戏内发送一条聊天消息，采集后重试。")

        message_info = {
            **message_template,
            "userName": user_name,
            "type": message_template.get("type") or "PUBLISH",
            "subType": message_template.get("subType") or "NORMAL",
            "content": content,
            "optional": optional,
        }
        message_info.setdefault(
            "attachmentInfo",
            {
                "metadataInfo": {
                    "playTime": 0,
                    "fileSize": 0,
                }
            },
        )
        message_info.setdefault(
            "senderProperties",
            {
                "hidden": False,
            },
        )

        return {
            "gameUserKey": {
                "characterId": str(character_id),
                "serverKey": str(server_key),
            },
            "gameMessageInfo": message_info,
            "validationInfo": validation_info,
            "extraData": extra_data,
        }

    def authorization(self) -> str | None:
        with self.lock:
            return self.data.get("authorization")

    def user_agent(self) -> str:
        with self.lock:
            return self.data.get("userAgent") or (
                "AION2/1.0.46.0.2026082601 (http-legacy) "
                "Windows/10.0.19045.1.256.64bit"
            )

    def api_headers(self) -> dict[str, str]:
        with self.lock:
            headers = dict(self.data.get("apiHeaders") or {})
        headers.setdefault("Lime-API-Version", "1")
        headers["Lime-Trace-Id"] = secrets.token_hex(16)
        return headers


runtime_state = RuntimeState()


def _send_whisper_http(
    character_id: str,
    server_key: str | None,
    content: str,
    room_key_info: Any = None,
    receiver_game_user_id: Any = None,
) -> dict[str, Any]:
    authorization = runtime_state.authorization()
    if not authorization:
        raise ValueError("缺少聊天 Bearer：请先完成 gameLogin/loginWithToken")

    body = runtime_state.build_whisper_body(
        character_id,
        server_key,
        content,
        room_key_info=room_key_info,
        receiver_game_user_id=receiver_game_user_id,
    )
    request_headers = {
        "Content-Type": "application/json; charset=utf-8",
        "Accept": "*/*",
        "Authorization": authorization,
        "User-Agent": runtime_state.user_agent(),
        **runtime_state.api_headers(),
    }
    body_bytes = json.dumps(body, ensure_ascii=False).encode("utf-8")
    _print_raw_whisper_request(body, request_headers, body_bytes)
    request = urllib.request.Request(
        SEND_WHISPER_URL,
        data=body_bytes,
        headers=request_headers,
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            raw = response.read().decode("utf-8", errors="replace")
            response_body = _try_json(raw)
            return {
                "ok": 200 <= response.status < 300,
                "status": response.status,
                "response": response_body,
                "request": _debug_whisper_request(body, request_headers),
            }
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        return {
            "ok": False,
            "status": exc.code,
            "response": _try_json(raw),
            "request": _debug_whisper_request(body, request_headers),
        }
    except urllib.error.URLError as exc:
        return {
            "ok": False,
            "status": None,
            "error": str(exc.reason),
        }


def _send_faction_http(content, expected_server=None):
    # Capture one coherent login snapshot; never accept a sender supplied by the portal.
    with runtime_state.lock:
        server = runtime_state.data.get('serverKey')
        origin = runtime_state.data.get('chatApiOrigin')
        authorization = runtime_state.authorization()
        if not server or not authorization or not origin or not runtime_state.data.get('userName'):
            raise ValueError('缺少当前角色或聊天接口信息，请先在游戏内发送一条聊天消息。')
        if expected_server and str(expected_server) != str(server):
            raise ValueError('当前角色服务器已变化，未发送，请重新选择客户端。')
        body = runtime_state.build_whisper_body('', str(server), content)
        body.pop('gameUserKey')
        body['gameRoomKeyInfo'] = {'type': 'WORLD', 'serverKey': str(server), 'roomKey': str(server)}
        body['gameMessageInfo']['type'] = 'PUBLISH'
        body['gameMessageInfo']['subType'] = 'NORMAL'
        headers = {'Content-Type': 'application/json; charset=utf-8', 'Accept': '*/*',
                   'Authorization': authorization, 'User-Agent': runtime_state.user_agent(),
                   **runtime_state.api_headers()}
    request = urllib.request.Request(origin + '/gameClient/sendMessage',
        data=json.dumps(body, ensure_ascii=False).encode('utf-8'), headers=headers, method='POST')
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            raw = response.read().decode('utf-8', errors='replace')
            parsed = _try_json(raw)
            business_error = isinstance(parsed, dict) and (
                parsed.get('ok') is False or parsed.get('success') is False or bool(parsed.get('error'))
                or parsed.get('code') not in (None, 0, '0', 200, '200', 'SUCCESS', 'OK'))
            # Only return the error code, never echo chat text, headers or account data.
            code = parsed.get('code') if isinstance(parsed, dict) else None
            safe_code = str(code)[:80] if isinstance(code, (str, int)) else None
            return {'ok': 200 <= response.status < 300 and not business_error, 'status': response.status, 'code': safe_code}
    except urllib.error.HTTPError as exc:
        return {'ok': False, 'status': exc.code}
    except urllib.error.URLError:
        return {'ok': False, 'status': None}


class MqttRelay:
    def __init__(self, storage_path: str | Path | None = None) -> None:
        self.loop: asyncio.AbstractEventLoop | None = None
        self.thread: threading.Thread | None = None
        self.mqtt = None
        self._mqtt_event_keys: set[str] = set()
        self._mqtt_event_order: deque[str] = deque(maxlen=1000)
        self.started_at = datetime.now(timezone.utc).isoformat()
        self.ready = threading.Event()
        self.stopped = threading.Event()
        self._mqtt_lock = threading.RLock()
        self._storage_path = storage_path
        self._db = None
        self._inflight: dict[int, str] = {}
        self._inflight_keys: set[str] = set()
        self._publish_acks = queue.SimpleQueue()
        self._flush_scheduled = False
        self._active_commands: set[str] = set()
        self._command_tasks: set = set()
        self._command_slots = None
        self._command_workers = max(1, min(64, int(os.getenv('AION2_COMMAND_WORKERS', str(min(32, (os.cpu_count() or 1) + 4))))))
        self._command_capacity = max(self._command_workers, int(os.getenv('AION2_COMMAND_CAPACITY', '128')))
        self._executor = ThreadPoolExecutor(max_workers=self._command_workers, thread_name_prefix='aion2-command')
        self._shutdown_started = False
        self._account_switch = None

    def account_switch(self):
        if self._account_switch is None:
            from account_switch import AccountSwitch
            self._account_switch = AccountSwitch(self, AGENT_ID)
        return self._account_switch

    def _database(self):
        # Called under _mqtt_lock. Delay opening until runtime configuration is known.
        if self._db is None:
            path = self._storage_path
            if path is None:
                scope = f'{MQTT_HOST}:{MQTT_PORT}|{MQTT_PREFIX}|{MQTT_ROOM}|{AGENT_ID}'
                folder = Path(os.getenv('AION2_RELAY_DATA_DIR', str(Path(os.getenv('LOCALAPPDATA', str(Path.home()))) / 'Aion2Relay')))
                folder.mkdir(parents=True, exist_ok=True)
                path = folder / (hashlib.sha256(scope.encode()).hexdigest()[:24] + '.sqlite3')
            self._db = sqlite3.connect(str(path), check_same_thread=False, timeout=10)
            self._db.execute('PRAGMA journal_mode=WAL')
            self._db.execute('PRAGMA synchronous=FULL')
            self._db.executescript('''
                CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, event_key TEXT NOT NULL UNIQUE, payload TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS commands (request_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, result TEXT, created_at REAL NOT NULL);
            ''')
        return self._db

    def pending_event_count(self):
        with self._mqtt_lock:
            return self._database().execute('SELECT COUNT(*) FROM events').fetchone()[0]

    @property
    def agent_topic(self) -> str:
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/agents/{AGENT_ID}/status"

    @property
    def discovery_topic(self) -> str:
        # Retain the MQTT discovery topic used by existing consoles.
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/signal/agent/all"

    @property
    def chat_event_topic(self) -> str:
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/events/{AGENT_ID}/chat"

    @property
    def receipt_event_topic(self) -> str:
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/events/{AGENT_ID}/receipts"

    def _event_topic(self, record: dict[str, Any]) -> str:
        if record.get('type') in ('control_ack', 'control_result', 'control_progress'):
            return self.receipt_event_topic
        if record.get('type') == 'agent_status':
            return self.agent_topic
        return self.chat_event_topic

    @property
    def control_topic(self) -> str:
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/control/agent/{AGENT_ID}"

    @property
    def broadcast_control_topic(self) -> str:
        return f"{MQTT_PREFIX}/{MQTT_ROOM}/control/agent/all"

    def start(self) -> None:
        if self.thread and self.thread.is_alive():
            return
        self.thread = threading.Thread(target=self._thread_main, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.stopped.set()

    def publish_chat(self, record: dict[str, Any]) -> None:
        if not self.loop or not self.loop.is_running() or self.stopped.is_set():
            self._publish_mqtt_event(record)
            return
        asyncio.run_coroutine_threadsafe(self._publish_record(record), self.loop)

    def _thread_main(self) -> None:
        relay_logger = logging.getLogger("aion2.relay")
        if not relay_logger.handlers:
            try:
                handler = RotatingFileHandler("aion2-relay.log", maxBytes=2_000_000, backupCount=2, encoding="utf-8")
                handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
                relay_logger.addHandler(handler)
                relay_logger.setLevel(logging.INFO)
            except OSError:
                logging.warning("[RELAY] cannot create connection log")
        self.loop = asyncio.ProactorEventLoop() if os.name == 'nt' else asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._main())
        except Exception:
            relay_logger.exception("communication thread failed")
        finally:
            self.ready.clear()
            self.loop.run_until_complete(self._shutdown())
            tasks = asyncio.all_tasks(self.loop)
            for task in tasks:
                task.cancel()
            if tasks:
                self.loop.run_until_complete(asyncio.gather(*tasks, return_exceptions=True))
            self.loop.close()
            self.loop = None

    async def _main(self) -> None:
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print(
                "[RELAY] 缺少 paho-mqtt，请执行: python -m pip install paho-mqtt",
                flush=True,
            )
            return

        if MQTT_HOST.endswith('.emqxsl.cn') and (not MQTT_USERNAME or not MQTT_PASSWORD):
            print('[RELAY] 私有 MQTT 尚未配置账号，请填写 mqtt.private.local.json 后重启客户端', flush=True)
            return

        client_id = f"aion2-agent-{AGENT_ID}-{uuid.uuid4().hex[:8]}"
        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        if MQTT_USERNAME:
            self.mqtt.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        self.mqtt.reconnect_delay_set(min_delay=1, max_delay=60)
        self.mqtt.max_queued_messages_set(500)
        self.mqtt.on_publish = self._on_mqtt_publish
        self.mqtt.max_inflight_messages_set(64)
        self.mqtt.on_connect_fail = self._on_mqtt_connect_fail

        self.mqtt.on_connect = self._on_mqtt_connect
        self.mqtt.on_message = self._on_mqtt_message
        self.mqtt.on_disconnect = self._on_mqtt_disconnect
        self.mqtt.will_set(
            self.agent_topic,
            json.dumps({**self._status_payload("offline"), "will": True}, ensure_ascii=False),
            qos=1,
            retain=True,
        )

        if MQTT_PORT in (8883, 8084):
            self.mqtt.tls_set()

        # loop_start retries the first connection as well as later disconnects.
        # A DNS/TLS/TCP failure at startup must not kill the relay thread.
        self.mqtt.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
        self.mqtt.loop_start()
        print(
            f"[RELAY] agent={AGENT_ID} room={MQTT_ROOM} mqtt={MQTT_HOST}:{MQTT_PORT}",
            flush=True,
        )

        while not self.stopped.is_set():
            try:
                if self.ready.is_set():
                    self._publish_status("online")
                    self._publish_pending_mqtt_events()
            except Exception:
                logging.exception("[RELAY] heartbeat failed; will retry")
            for _ in range(10):
                if self.stopped.is_set():
                    break
                await asyncio.sleep(0.5)

        await self._shutdown()

    async def _shutdown(self) -> None:
        if self._shutdown_started:
            return
        self._shutdown_started = True
        if self._account_switch is not None:
            await self._account_switch.close()
        if self._command_tasks:
            await asyncio.gather(*list(self._command_tasks), return_exceptions=True)
        self._executor.shutdown(wait=True)
        self.ready.clear()
        if self.mqtt:
            self._publish_status("offline")
            self.mqtt.disconnect()
            self.mqtt.loop_stop()
            self.mqtt = None
        with self._mqtt_lock:
            self._inflight.clear()
            self._inflight_keys.clear()
            if self._db is not None:
                self._db.close()
                self._db = None

    def _on_mqtt_connect(self, client, userdata, flags, reason_code, properties=None) -> None:
        if reason_code != 0:
            self.ready.clear()
            print(f"[RELAY] MQTT connection rejected: {reason_code}", flush=True)
            return
        client.subscribe(self.discovery_topic)
        client.subscribe(self.control_topic)
        client.subscribe(self.broadcast_control_topic)
        self.ready.set()
        # Do not run backlog publishing or state locks on Paho's network thread.
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(self._publish_status, "online")
            self.loop.call_soon_threadsafe(self._publish_pending_mqtt_events)
        print("[RELAY] MQTT connected, ready for console commands", flush=True)
        logging.getLogger("aion2.relay").info("connected agent=%s broker=%s:%s build=%s", AGENT_ID, MQTT_HOST, MQTT_PORT, ADDON_BUILD)

    def _on_mqtt_disconnect(self, client, userdata, flags, reason_code, properties=None) -> None:
        self.ready.clear()
        print(f"[RELAY] MQTT disconnected: {reason_code}", flush=True)
        logging.getLogger("aion2.relay").warning("disconnected reason=%s; automatic reconnect enabled", reason_code)

    def _on_mqtt_connect_fail(self, client, userdata) -> None:
        self.ready.clear()
        print("[RELAY] MQTT connection failed; automatic retry in 1-60 seconds", flush=True)
        logging.getLogger("aion2.relay").warning("connection attempt failed broker=%s:%s; retry enabled", MQTT_HOST, MQTT_PORT)

    def _on_mqtt_message(self, client, userdata, msg) -> None:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return

        if not isinstance(payload, dict):
            return

        if payload.get("sender") == AGENT_ID:
            return
        if payload.get("target") not in (AGENT_ID, "all", None):
            return

        message_type = payload.get("type")
        if message_type in {"switchAccount", "switchAccountStatus", "cancelSwitchAccount", "resumeAccountChat"}:
            if payload.get("target") != AGENT_ID or msg.topic != self.control_topic or getattr(msg, 'retain', False):
                return
            if self.loop:
                asyncio.run_coroutine_threadsafe(self._handle_control_message(json.dumps(payload, ensure_ascii=False)), self.loop)
            return
        if message_type == "discover":
            if self.loop and self.loop.is_running():
                self.loop.call_soon_threadsafe(self._publish_status, "online")
        elif message_type in {"ping", "requestStatus", "sendWhisper", "sendFactionMessage"} and self.loop:
            asyncio.run_coroutine_threadsafe(
                self._handle_control_message(json.dumps(payload, ensure_ascii=False)),
                self.loop,
            )

    def _publish_status(self, status: str) -> None:
        if not self.mqtt or not self.mqtt.is_connected():
            return
        info = self.mqtt.publish(
            self.agent_topic,
            json.dumps(self._status_payload(status), ensure_ascii=False),
            # A heartbeat is replaceable. Do not queue obsolete heartbeats
            # throughout an outage and flood them on reconnection.
            qos=0 if status == "online" else 1,
            retain=True,
        )
        if status == "offline":
            try:
                info.wait_for_publish(timeout=1)
            except RuntimeError:
                pass

    def _record_key(self, record: dict[str, Any]) -> str:
        return "|".join(
            str(part or "")
            for part in (
                record.get("type"),
                record.get("message_id"),
                record.get("requestId"),
                record.get("command"),
                record.get("time"),
                record.get("url"),
                record.get("summary"),
            )
        )

    def _remember_mqtt_event(self, key: str) -> bool:
        if key in self._mqtt_event_keys:
            return False
        if len(self._mqtt_event_order) == self._mqtt_event_order.maxlen:
            old_key = self._mqtt_event_order.popleft()
            self._mqtt_event_keys.discard(old_key)
        self._mqtt_event_order.append(key)
        self._mqtt_event_keys.add(key)
        return True

    def _publish_mqtt_event(self, record: dict[str, Any]) -> None:
        key = self._record_key(record)
        payload = dict(record)
        payload.setdefault("agentId", AGENT_ID)
        with self._mqtt_lock:
            if key in self._mqtt_event_keys:
                return
            db = self._database()
            with db:
                db.execute('INSERT OR IGNORE INTO events(event_key, payload) VALUES (?, ?)',
                           (key, json.dumps(payload, ensure_ascii=False)))
        self._publish_pending_mqtt_events()

    def _schedule_flush(self):
        with self._mqtt_lock:
            if self._flush_scheduled or not self.loop or not self.loop.is_running():
                return
            self._flush_scheduled = True
        self.loop.call_soon_threadsafe(self._flush_from_callback)

    def _flush_from_callback(self):
        with self._mqtt_lock:
            self._flush_scheduled = False
        self._publish_pending_mqtt_events()

    def _on_mqtt_publish(self, client, userdata, mid, reason_code=None, properties=None):
        # Paho may hold its outgoing-message mutex while invoking this callback.
        # Never acquire our publish lock here: publish() takes those locks in reverse order.
        self._publish_acks.put((mid, not getattr(reason_code, 'is_failure', False)))
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(self._drain_publish_acks)
        else:
            self._drain_publish_acks()

    def _drain_publish_acks(self):
        acknowledged = []
        with self._mqtt_lock:
            while not self._publish_acks.empty():
                mid, success = self._publish_acks.get()
                key = self._inflight.pop(mid, None)
                if key is not None:
                    self._inflight_keys.discard(key)
                    if success:
                        acknowledged.append(key)
            if acknowledged:
                db = self._database()
                with db:
                    db.executemany('DELETE FROM events WHERE event_key = ?', [(key,) for key in acknowledged])
                for key in acknowledged:
                    self._remember_mqtt_event(key)
        self._schedule_flush()

    def _publish_pending_mqtt_events(self) -> None:
        with self._mqtt_lock:
            if not self.mqtt or not self.mqtt.is_connected():
                return
            remaining = 128 - len(self._inflight)
            if remaining <= 0:
                return
            rows = self._database().execute('SELECT event_key, payload FROM events ORDER BY id LIMIT 256').fetchall()
            for key, payload in rows:
                if key in self._inflight_keys:
                    continue
                try:
                    # Hold the lock through MID registration: on_publish runs on Paho's thread.
                    info = self.mqtt.publish(self._event_topic(json.loads(payload)), payload, qos=1, retain=False)
                    if int(info.rc) in (0, 4, 7):
                        # Paho owns QoS 1 retransmission even if the connection just disappeared.
                        self._inflight[info.mid] = key
                        self._inflight_keys.add(key)
                        remaining -= 1
                    else:
                        break  # Rejected before enqueueing; durable row remains for retry.
                except Exception:
                    logging.exception('[RELAY] MQTT publish failed; durable event retained')
                    break
                if remaining <= 0:
                    break

    def _status_payload(self, status: str) -> dict[str, Any]:
        identity = runtime_state.client_identity()
        game_status_path = os.getenv('AION2_GAME_SERVER_STATUS')
        if game_status_path:
            # The parent supplies fresh game-connection identity independently of
            # chat authentication. Never mutate the latter or invent a character.
            server = None
            try:
                with open(game_status_path, encoding='utf-8') as file:
                    game_status = json.loads(file.read(4096))
                age = time.time() * 1000 - game_status['updatedAt']
                candidate = game_status.get('serverId')
                if -5000 <= age <= 15000 and isinstance(candidate, str) and candidate.isascii() and candidate.isdecimal() and 0 < int(candidate) <= 65535:
                    server = candidate
            except (OSError, ValueError, TypeError, KeyError):
                pass
            if str(identity.get('serverId')) != server:
                identity['characterName'] = None
            identity['serverId'] = server
        return {
            "type": "agent_status",
            "status": status,
            "agentId": AGENT_ID,
            "host": platform.node() or socket.gethostname(),
            "serverId": identity.get("serverId"),
            "characterName": identity.get("characterName"),
            "room": MQTT_ROOM,
            "startedAt": self.started_at,
            "build": ADDON_BUILD,
            "accountSwitchProtocol": 1,
            "time": datetime.now(timezone.utc).isoformat(),
        }

    def _receipt(self, request_id, ok=False, status='failed', error='', **extra):
        return {'type': 'control_result', 'requestId': request_id, 'command': 'sendWhisper',
                'agentId': AGENT_ID, 'ok': ok, 'status': status, 'error': error,
                'time': datetime.now(timezone.utc).isoformat(), **extra}

    async def _handle_control_message(self, message: Any) -> None:
        try:
            payload = json.loads(message)
        except (ValueError, TypeError, UnicodeDecodeError):
            return
        if not isinstance(payload, dict):
            return
        if payload.get('type') in ('switchAccount', 'switchAccountStatus', 'cancelSwitchAccount'):
            await self.account_switch().handle(payload)
        elif payload.get('type') == 'resumeAccountChat':
            await self.account_switch().resume_chat(payload, runtime_state.client_identity())
        elif payload.get('type') in ('sendWhisper', 'sendFactionMessage'):
            await self._dispatch_whisper(payload)
        elif payload.get('type') in ('ping', 'requestStatus'):
            self._publish_status('online')

    async def _dispatch_whisper(self, payload):
        receipt = lambda *args, **kwargs: self._receipt(*args, command=payload.get('type', 'sendWhisper'), **kwargs)
        if self.account_switch().chat_blocked():
            await self._publish_record(receipt(payload.get('requestId'), status='not_sent', error='换号后聊天处于暂停状态，请在控制台核对当前角色并恢复聊天。'))
            return
        request_id = payload.get('requestId')
        if not isinstance(request_id, str) or not request_id or len(request_id) > 200:
            # Unidentifiable commands cannot be executed safely under retries.
            return
        semantic = {k: v for k, v in payload.items() if k not in ('requestId', 'time', 'sender', 'sessionId', 'target', 'expiresAt')}
        fingerprint = hashlib.sha256(json.dumps(semantic, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        with self._mqtt_lock:
            db = self._database()
            with db:
                claimed = db.execute('INSERT OR IGNORE INTO commands VALUES (?, ?, NULL, ?)',
                                     (request_id, fingerprint, time.time())).rowcount == 1
                row = None if claimed else db.execute(
                    'SELECT fingerprint, result FROM commands WHERE request_id = ?', (request_id,)).fetchone()
        if row is not None:
            if row[0] != fingerprint:
                await self._publish_record(receipt(request_id, error='同一请求编号的内容不一致，拒绝执行。'))
            elif row[1] is not None:
                # Fresh delivery identity; return the saved outcome without executing again.
                await self._publish_record({**json.loads(row[1]), 'time': datetime.now(timezone.utc).isoformat()})
            elif request_id not in self._active_commands:
                result = receipt(request_id, status='unknown', error='上次执行被中断，结果未确认；请核对聊天记录，不会自动重发。')
                self._finish_command(request_id, result)
                await self._publish_record(result)
            return
        if self._shutdown_started or len(self._active_commands) >= self._command_capacity:
            result = receipt(request_id, status='not_sent', error='客户端正在退出或任务队列已满，本次未执行。')
            self._finish_command(request_id, result)
            await self._publish_record(result)
            return
        self._active_commands.add(request_id)
        task = asyncio.current_task()
        self._command_tasks.add(task)
        try:
            await self._publish_record({'type': 'control_ack', 'requestId': request_id,
                'command': payload.get('type', 'sendWhisper'), 'agentId': AGENT_ID, 'ok': True,
                'time': datetime.now(timezone.utc).isoformat()})
            if self._command_slots is None:
                self._command_slots = asyncio.Semaphore(self._command_workers)
            expires = payload.get('expiresAt')
            if not isinstance(expires, (int, float)) or isinstance(expires, bool):
                expires = time.time() * 1000 + 30000
            remaining = min(30.0, (expires - time.time() * 1000) / 1000)
            if remaining <= 0:
                result = receipt(request_id, status='not_sent', error='指令已过期，未执行。')
            else:
                try:
                    await asyncio.wait_for(self._command_slots.acquire(), timeout=remaining)
                except asyncio.TimeoutError:
                    result = receipt(request_id, status='not_sent', error='排队超时，未执行。')
                else:
                    try:
                        if time.time() * 1000 >= expires:
                            result = receipt(request_id, status='not_sent', error='指令已过期，未执行。')
                        else:
                            result = await (self._execute_faction(payload) if payload.get('type') == 'sendFactionMessage' else self._execute_whisper(payload))
                    finally:
                        self._command_slots.release()
            self._finish_command(request_id, result)
            await self._publish_record(result)
        finally:
            self._active_commands.discard(request_id)
            self._command_tasks.discard(task)

    def _finish_command(self, request_id, result):
        with self._mqtt_lock:
            db = self._database()
            with db:
                db.execute('UPDATE commands SET result = ? WHERE request_id = ?',
                           (json.dumps(result, ensure_ascii=False), request_id))
                db.execute('INSERT OR IGNORE INTO events(event_key, payload) VALUES (?, ?)',
                           (self._record_key(result), json.dumps(result, ensure_ascii=False)))

    async def _execute_faction(self, payload):
        request_id = payload['requestId']
        receipt = lambda **kwargs: self._receipt(request_id, command='sendFactionMessage', **kwargs)
        if self.account_switch().chat_blocked():
            return receipt(status='not_sent', error='换号期间聊天已暂停，未发送。')
        content = payload.get('content')
        if not isinstance(content, str) or not content.strip():
            return receipt(status='not_sent', error='阵营消息不能为空。')
        try:
            result = await asyncio.get_running_loop().run_in_executor(
                self._executor, _send_faction_http, content.strip(), payload.get('serverKey'))
        except ValueError as exc:
            return receipt(status='not_sent', error=str(exc))
        except Exception:
            return receipt(status='unknown', error='游戏请求结果未确认，请核对聊天记录，勿重复发送。')
        ok = result.get('ok') is True
        return receipt(ok=ok, status='confirmed' if ok else 'failed' if result.get('status') else 'unknown',
            result=result, error='' if ok else '阵营消息发送未确认（HTTP %s，错误码 %s），请核对游戏聊天记录。' % (result.get('status'), result.get('code') or '无'))

    async def _execute_whisper(self, payload):
        request_id = payload['requestId']
        character_id = payload.get('characterId') or payload.get('targetCharacterId')
        server_key = payload.get('serverKey') or payload.get('targetServerKey')
        content = payload.get('content')
        if not character_id or not content:
            return self._receipt(request_id, status='not_sent', error='缺少 characterId 或 content，未执行。')
        runtime_state.apply_self_profile(payload.get('selfProfile'))
        try:
            result = await asyncio.get_running_loop().run_in_executor(self._executor, _send_whisper_http,
                str(character_id), str(server_key) if server_key not in (None, '') else None,
                str(content), payload.get('gameRoomKeyInfo'), payload.get('receiverGameUserId'))
        except ValueError as exc:
            return self._receipt(request_id, status='not_sent', error=str(exc))
        except Exception as exc:
            result = {'ok': False, 'error': str(exc)}
        ok = bool(result.get('ok'))
        status = 'confirmed' if ok else 'failed' if result.get('status') is not None else 'unknown'
        return self._receipt(request_id, ok=ok, status=status, result=result,
            error='' if ok else '游戏请求未确认，请核对聊天记录。' if status == 'unknown' else '游戏接口返回失败。',
            target={'characterId': str(character_id), 'serverKey': str(server_key) if server_key else None})

    async def _publish_record(self, record: dict[str, Any]) -> None:
        if record.get('type') in ('control_ack', 'agent_status'):
            # Replaceable progress notifications: only chat and final outcomes need durable delivery.
            # Persisting each intermediate ACK adds disk commits without strengthening execution safety.
            if self.mqtt and self.mqtt.is_connected():
                self.mqtt.publish(self._event_topic(record), json.dumps(record, ensure_ascii=False), qos=0, retain=False)
            return
        self._publish_mqtt_event(record)


relay = MqttRelay()


class WsMessageMonitor:
    def __init__(self) -> None:
        self._poll_task: asyncio.Task | None = None
        self._seen_ws_counts: dict[str, int] = {}
        self._seen_ws_flows: set[str] = set()

    def load(self, loader) -> None:
        print(f"[ADDON] build={ADDON_BUILD} file={__file__}", flush=True)
        relay.start()

    def running(self) -> None:
        self._poll_task = asyncio_utils.create_task(
            self._poll_websocket_store(),
            name="aion2-ws-store-poller",
        )

    def done(self) -> None:
        if self._poll_task:
            self._poll_task.cancel()
            self._poll_task = None
        relay.stop()

    def tls_clienthello(self, data: Any) -> None:
        sni = getattr(data.client_hello, "sni", None)
        if not _is_plaync_host(sni):
            # Aion2Pipe forwards AION2.exe TCP/443 so the SNI can be inspected.
            # Only Lime/PlayNC traffic belongs to the chat feature. Everything
            # else must remain an untouched TLS tunnel: intercepting analytics,
            # device, campaign, or SDK endpoints can break their private CA or
            # certificate-pinning policy and surface game error 100199:502.
            data.ignore_connection = True
            return
        print(f"[TLS PLAYNC] sni={sni} {_client_info(data.context)}", flush=True)

    def tls_established_client(self, data: Any) -> None:
        sni = getattr(data.context.client, "sni", None)
        if _is_plaync_host(sni):
            print(
                f"[TLS OK CLIENT] sni={sni} {_client_info(data.context)}",
                flush=True,
            )

    def tls_failed_client(self, data: Any) -> None:
        sni = getattr(data.context.client, "sni", None)
        if _is_plaync_host(sni):
            print(
                f"[TLS FAIL CLIENT] sni={sni} error={data.conn.error} "
                f"{_client_info(data.context)}",
                flush=True,
            )

    def request(self, flow: http.HTTPFlow) -> None:
        if _flow_plaync_host(flow):
            print(
                f"[HTTP PLAYNC] {flow.request.method} {flow.request.pretty_url} "
                f"{_client_info(flow)}",
                flush=True,
            )
        runtime_state.observe_request(flow)
        self._print_chat_api_request(flow)

    def response(self, flow: http.HTTPFlow) -> None:
        runtime_state.observe_response(flow)

    def websocket_start(self, flow: http.HTTPFlow) -> None:
        print(
            f"[WS OPEN] host={flow.request.host} plaync={_flow_plaync_host(flow)} "
            f"path={flow.request.path} {_client_info(flow)}",
            flush=True,
        )
        if self._is_target_ws(flow):
            print(
                f"[WS] connected host={flow.request.host} path={flow.request.path}",
                flush=True,
            )
        elif self._looks_like_chat_ws(flow):
            print(
                f"[WS] other plaync websocket host={flow.request.host} path={flow.request.path}",
                flush=True,
            )

    def websocket_message(self, flow: http.HTTPFlow) -> None:
        if not self._is_target_ws(flow):
            if self._looks_like_chat_ws(flow):
                print(
                    f"[WS SKIP] host={flow.request.host} path={flow.request.path}",
                    flush=True,
                )
            return
        if flow.websocket is None or not flow.websocket.messages:
            return

        message = flow.websocket.messages[-1]
        self._seen_ws_counts[flow.id] = len(flow.websocket.messages)
        self._process_ws_message(flow, message, source="hook")

    async def _poll_websocket_store(self) -> None:
        print("[WS POLLER] started: scanning mitmweb flow store", flush=True)
        while True:
            try:
                await asyncio.sleep(0.5)
                view = ctx.master.addons.get("view")
                if view is None:
                    continue

                store = getattr(view, "_store", None)
                flows = list(store.values()) if store is not None else list(view)
                for flow in flows:
                    websocket = getattr(flow, "websocket", None)
                    if websocket is None:
                        continue

                    if flow.id not in self._seen_ws_flows:
                        self._seen_ws_flows.add(flow.id)
                        print(
                            f"[WS STORE FLOW] flow={flow.id} host={flow.request.host} "
                            f"path={flow.request.path} messages={len(websocket.messages)}",
                            flush=True,
                        )

                    if not websocket.messages:
                        continue

                    seen_count = self._seen_ws_counts.get(flow.id, 0)
                    messages = websocket.messages
                    if seen_count >= len(messages):
                        continue

                    print(
                        f"[WS STORE] flow={flow.id} host={flow.request.host} "
                        f"path={flow.request.path} new={len(messages) - seen_count} "
                        f"total={len(messages)}",
                        flush=True,
                    )
                    for message in messages[seen_count:]:
                        self._process_ws_message(flow, message, source="store")
                    self._seen_ws_counts[flow.id] = len(messages)
            except asyncio.CancelledError:
                raise
            except Exception:
                print("[WS POLLER] error:", flush=True)
                print(traceback.format_exc(), flush=True)
                await asyncio.sleep(2)

    def _process_ws_message(self, flow: http.HTTPFlow, message: Any, source: str) -> None:
        direction = "C->S" if message.from_client else "S->C"
        raw = _decode_message(message.content)
        frames = _parse_stomp_frames(raw)
        if not frames:
            if raw.strip():
                print(f"[WS RAW {direction} {source}] {raw}", flush=True)
            return

        for frame in frames:
            destination = frame["headers"].get("destination", "")
            body = frame["body"]
            body_preview = body.strip()
            print(
                f"[STOMP {direction} {source}] {frame['command']} "
                f"destination={destination} body_len={len(body)}",
                flush=True,
            )
            if frame["command"] not in {"MESSAGE", "SEND"}:
                if body_preview:
                    parsed_body = _expand_nested_json(_try_json(body))
                    print(
                        json.dumps(parsed_body, ensure_ascii=False, indent=2)
                        if isinstance(parsed_body, (dict, list))
                        else body_preview,
                        flush=True,
                    )
                continue

            payload = _expand_nested_json(_try_json(body))
            chat_meta = _extract_chat_meta(payload)
            if not chat_meta.get("content"):
                chat_meta = _extract_request_chat_meta(payload, flow.request.pretty_url)
            runtime_state.observe_chat(payload, chat_meta, direction=direction)
            record = {
                "type": "chat_message",
                "time": datetime.now(timezone.utc).isoformat(),
                "stomp_command": frame["command"],
                "direction": direction,
                "source": source,
                "destination": frame["headers"].get("destination"),
                "message_id": frame["headers"].get("message-id"),
                "summary": _summarize_chat(payload),
                "chat_meta": chat_meta,
                "payload": payload,
            }

            OUTPUT_FILE.open("a", encoding="utf-8").write(
                json.dumps(record, ensure_ascii=False) + "\n"
            )
            summary = _summarize_chat(payload)
            print(f"[CHAT] {summary}", flush=True)
            print(
                json.dumps(payload, ensure_ascii=False, indent=2),
                flush=True,
            )
            relay.publish_chat(record)
            logging.info("[CHAT] %s", summary)

    def _is_target_ws(self, flow: http.HTTPFlow) -> bool:
        path = (flow.request.path or "").split("?", 1)[0].lower()
        return bool(_flow_plaync_host(flow)) and path.endswith("/stomp")

    def _looks_like_chat_ws(self, flow: http.HTTPFlow) -> bool:
        path = (flow.request.path or "").lower()
        return bool(_flow_plaync_host(flow)) and ("stomp" in path or "chat" in path)

    def _print_chat_api_request(self, flow: http.HTTPFlow) -> None:
        path = flow.request.path or ""
        if not _flow_plaync_host(flow) or CHAT_API_PREFIX not in path:
            return
        if not any(name in path for name in ("sendMessage", "sendWhisper")):
            return

        body = _expand_nested_json(_json_body(flow))
        if body is None:
            return

        url = _effective_flow_url(flow)
        chat_meta = _extract_request_chat_meta(body, url)
        content = chat_meta.get("content") or ""
        sender = chat_meta.get("sender") or "unknown"
        command = "sendWhisper" if "sendWhisper" in path else "sendMessage"
        summary = f"[发送请求] {command} {sender}: {content}"
        record = {
            "type": "chat_request",
            "time": datetime.now(timezone.utc).isoformat(),
            "url": url,
            "summary": summary,
            "chat_meta": chat_meta,
            "payload": body,
        }

        print(f"[HTTP CHAT OUT] {flow.request.method} {url}", flush=True)
        print(f"[CHAT] {summary}", flush=True)
        print(json.dumps(body, ensure_ascii=False, indent=2), flush=True)
        # The server echoes successful sends over STOMP. Publish only that
        # server-confirmed chat_message; publishing this request as well makes
        # one in-game message appear twice and can display rejected sends.


addons = [WsMessageMonitor()]
