"""Async Windows named-pipe client; no game automation and no third-party packages."""
import asyncio
import time
import uuid

from login_protocol import PIPE_NAME, Decoder, ProtocolError, RemoteError, encode, validate_snapshot, TERMINAL


class _Connection(asyncio.Protocol):
    def __init__(self, owner):
        self.owner = owner
        self.transport = None
        self.pending = {}
        self.decoder = Decoder()
        self.failure = None
        self.closed = asyncio.get_running_loop().create_future()
        self.ready = asyncio.get_running_loop().create_future()

    def connection_made(self, transport):
        self.transport = transport
        self.ready.set_result(None)

    def data_received(self, data):
        try:
            for message in self.decoder.feed(data):
                if message["type"] == "response":
                    future = self.pending.get(message["requestId"])
                    if future and not future.done():
                        if message["ok"]:
                            future.set_result(message["result"])
                        else:
                            future.set_exception(RemoteError(message["error"]))
                elif message["type"] == "event":
                    self.owner._remember({**message["data"], "taskId": message["taskId"], "seq": message["seq"]})
                else:
                    raise ProtocolError("Server sent a request")
        except (ProtocolError, TypeError) as error:
            self.failure = error
            self.transport.abort()

    def connection_lost(self, error):
        failure = self.failure or error or ConnectionError("Login process disconnected")
        for future in self.pending.values():
            if not future.done():
                future.set_exception(failure)
        self.pending.clear()
        self.transport = None
        if not self.closed.done():
            self.closed.set_result(None)

    async def request(self, method, params, timeout):
        if not self.transport or self.transport.is_closing():
            raise ConnectionError("Login process disconnected")
        if len(self.pending) >= 32 or self.transport.get_write_buffer_size() > 131072:
            raise ConnectionError("Login IPC queue is full")
        request_id = str(uuid.uuid4())
        frame = encode({"version": 1, "type": "request", "requestId": request_id, "method": method, "params": params})
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        try:
            self.transport.write(frame)
            return await asyncio.wait_for(future, timeout)
        finally:
            self.pending.pop(request_id, None)


class LoginBridge:
    """Use within one Windows Proactor event loop. Persist taskId before start().

    A request timeout has an UNKNOWN outcome: query status with the same taskId.
    The bridge never automatically replays start/cancel after an uncertain result.
    """
    def __init__(self, pipe=PIPE_NAME, request_timeout=5.0):
        if not pipe.startswith("\\\\.\\pipe\\") or "\\" in pipe[len("\\\\.\\pipe\\"):]:
            raise ValueError("Only local Windows pipe names are allowed")
        self.pipe = pipe
        self.request_timeout = request_timeout
        self._connection = None
        self._lock = asyncio.Lock()
        self._closed = False
        self.snapshots = {}
        self.events = asyncio.Queue(maxsize=256)
        self.dropped_events = 0
        self.server = None

    def _remember(self, snapshot):
        validate_snapshot(snapshot)
        old = self.snapshots.get(snapshot["taskId"])
        if old and snapshot["seq"] <= old["seq"]:
            return old
        if old and old["state"] in TERMINAL:
            raise ProtocolError("Server changed a terminal task")
        if len(self.snapshots) >= 512 and snapshot["taskId"] not in self.snapshots:
            self.snapshots.pop(next(iter(self.snapshots)))
        self.snapshots[snapshot["taskId"]] = snapshot
        if self.events.full():
            self.events.get_nowait()
            self.dropped_events += 1
        self.events.put_nowait(snapshot)
        return snapshot

    async def connect(self):
        async with self._lock:
            if self._closed:
                raise RuntimeError("Bridge is closed")
            current = self._connection
            if current and current.transport and not current.transport.is_closing():
                return current
            loop = asyncio.get_running_loop()
            if not hasattr(loop, "create_pipe_connection"):
                raise RuntimeError("Use the Windows Proactor event loop (asyncio.run default on Windows)")
            transport, connection = await asyncio.wait_for(
                loop.create_pipe_connection(lambda: _Connection(self), self.pipe), self.request_timeout)
            try:
                await asyncio.wait_for(connection.ready, self.request_timeout)
                server = await connection.request("hello", {"client": "aion2-client", "versions": [1]}, self.request_timeout)
                required = {"login.start", "login.status", "login.cancel", "ping"}
                if server.get("version") != 1 or not required.issubset(server.get("methods", [])):
                    raise ProtocolError("Incompatible login service")
            except BaseException:
                transport.abort()
                await connection.closed
                raise
            self.server = server
            self._connection = connection
            return connection

    async def request(self, method, params=None):
        connection = await self.connect()
        return await connection.request(method, params or {}, self.request_timeout)

    async def start(self, *, task_id, account_ref, server_id, character_id, timeout_seconds=180, _method="login.start"):
        for value in (task_id, account_ref, server_id, character_id):
            if not isinstance(value, str) or not value.strip() or len(value) > 128:
                raise ValueError("IDs must be non-empty strings up to 128 characters")
        if type(timeout_seconds) is not int or not 10 <= timeout_seconds <= 900:
            raise ValueError("timeout_seconds must be 10..900")
        await self.connect()
        if _method not in self.server["methods"]:
            raise RemoteError({"code": "METHOD_NOT_FOUND", "message": "登录进程未实现换号协议"})
        result = await self.request(_method, {"taskId": task_id, "accountRef": account_ref,
            "serverId": server_id, "characterId": character_id, "timeoutSeconds": timeout_seconds})
        return self._task_result(task_id, result)

    async def switch_account(self, **params):
        return await self.start(**params, _method="login.switch")

    def _task_result(self, task_id, result):
        if result.get("taskId") != task_id:
            raise ProtocolError("Task ID mismatch")
        return self._remember(result)

    async def status(self, task_id):
        return self._task_result(task_id, await self.request("login.status", {"taskId": task_id}))

    async def cancel(self, task_id):
        return self._task_result(task_id, await self.request("login.cancel", {"taskId": task_id}))

    async def ping(self):
        return await self.request("ping")

    async def wait(self, task_id, timeout=240, poll_interval=1.0):
        """Reconnect and query status; never resubmit a login. Timeout does not cancel it."""
        deadline = time.monotonic() + timeout
        delay = poll_interval
        while time.monotonic() < deadline:
            try:
                async with asyncio.timeout(max(0.01, deadline - time.monotonic())):
                    snapshot = await self.status(task_id)
                if snapshot["state"] in TERMINAL or snapshot["state"] == "waiting_user":
                    return snapshot
                delay = poll_interval
            except (OSError, ConnectionError, TimeoutError):
                delay = min(max(delay * 2, 0.1), 5.0)
            await asyncio.sleep(min(max(delay, 0.01), max(0, deadline - time.monotonic())))
        raise TimeoutError("Waiting expired; task outcome is unknown. Query the same taskId later.")

    async def close(self):
        async with self._lock:
            self._closed = True
            if self._connection and self._connection.transport:
                self._connection.transport.abort()
                await self._connection.closed

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *_):
        await self.close()
