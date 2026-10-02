"""Language-neutral AION2 Login IPC v1: uint32 LE length + UTF-8 JSON."""
import json
import struct

PIPE_NAME = r"\\.\pipe\aion2-login-v1"
MAX_FRAME = 65536
STATES = {"queued", "running", "waiting_user", "succeeded", "failed", "cancelled"}
TERMINAL = {"succeeded", "failed", "cancelled"}


class ProtocolError(ValueError):
    pass


class RemoteError(Exception):
    def __init__(self, error):
        self.code = error["code"]
        self.message = error["message"]
        super().__init__(f"{self.code}: {self.message}")


def validate(message):
    if not isinstance(message, dict) or type(message.get("version")) is not int or message["version"] != 1:
        raise ProtocolError("Unsupported protocol version")
    kind = message.get("type")
    if kind not in {"request", "response", "event"}:
        raise ProtocolError("Invalid message type")
    if kind in {"request", "response"}:
        _text(message.get("requestId"), "requestId")
    if kind == "request":
        _text(message.get("method"), "method")
        if not isinstance(message.get("params"), dict):
            raise ProtocolError("params must be an object")
    if kind == "response":
        if type(message.get("ok")) is not bool:
            raise ProtocolError("ok must be boolean")
        body = message.get("result" if message["ok"] else "error")
        if not isinstance(body, dict):
            raise ProtocolError("Missing result/error object")
        if not message["ok"]:
            _text(body.get("code"), "error.code")
            _text(body.get("message"), "error.message", 2000)
    if kind == "event":
        if message.get("method") != "login.event":
            raise ProtocolError("Unsupported event")
        validate_snapshot({**message.get("data", {}), "taskId": message.get("taskId"), "seq": message.get("seq")})
    return message


def _text(value, field, limit=128):
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        raise ProtocolError(f"Invalid {field}")


def validate_snapshot(value):
    if not isinstance(value, dict):
        raise ProtocolError("Invalid task snapshot")
    _text(value.get("taskId"), "taskId")
    if type(value.get("seq")) is not int or value["seq"] < 1:
        raise ProtocolError("Invalid task sequence")
    if value.get("state") not in STATES:
        raise ProtocolError("Invalid task state")
    if value["state"] == "failed":
        error = value.get("error")
        if not isinstance(error, dict):
            raise ProtocolError("Failed task requires error")
        _text(error.get("code"), "error.code")
        _text(error.get("message"), "error.message", 2000)
    if value["state"] == "succeeded":
        result = value.get("result")
        if not isinstance(result, dict):
            raise ProtocolError("Succeeded task requires result")
        for key in ("accountRef", "serverId", "characterId"):
            _text(result.get(key), key)
        if type(result.get("gamePid")) is not int or result["gamePid"] <= 0:
            raise ProtocolError("Invalid gamePid")
    return value


def encode(message):
    validate(message)
    body = json.dumps(message, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if not 0 < len(body) <= MAX_FRAME:
        raise ProtocolError("Frame exceeds 65536 bytes")
    return struct.pack("<I", len(body)) + body


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ProtocolError("Duplicate JSON key")
        result[key] = value
    return result


def _constant(_):
    raise ProtocolError("Non-finite JSON number")


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        messages = []
        while len(self.buffer) >= 4:
            size = struct.unpack_from("<I", self.buffer)[0]
            if not 0 < size <= MAX_FRAME:
                raise ProtocolError("Invalid frame length")
            if len(self.buffer) < size + 4:
                break
            body = bytes(self.buffer[4:4 + size])
            del self.buffer[:4 + size]
            try:
                message = json.loads(body.decode("utf-8"), object_pairs_hook=_pairs, parse_constant=_constant)
                messages.append(validate(message))
            except (ValueError, TypeError, RecursionError) as error:
                raise ProtocolError("Invalid JSON message") from error
        return messages
