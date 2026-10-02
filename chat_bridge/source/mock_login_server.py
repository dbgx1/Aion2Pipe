"""LOCAL TEST ONLY. Simulates login; never opens a game or handles credentials.

Uses asyncio's default pipe permissions and volatile task storage, so this is
not a production server. The real server must implement the ACL/durability rules.
"""
import asyncio
import json
import sys
import uuid
import _winapi
from asyncio import windows_events, windows_utils

from login_protocol import PIPE_NAME, Decoder, encode


class _BytePipeServer(windows_events.PipeServer):
    # Test-only adaptation: asyncio's convenience server defaults to MESSAGE
    # mode. Exercise the contract's BYTE mode, including large split frames.
    def _server_pipe_handle(self, first):
        if self.closed():
            return None
        flags = _winapi.PIPE_ACCESS_DUPLEX | _winapi.FILE_FLAG_OVERLAPPED
        if first:
            flags |= _winapi.FILE_FLAG_FIRST_PIPE_INSTANCE
        handle = _winapi.CreateNamedPipe(self._address, flags,
            0x8,  # PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
            _winapi.PIPE_UNLIMITED_INSTANCES, 65536, 65536,
            _winapi.NMPWAIT_WAIT_FOREVER, _winapi.NULL)
        pipe = windows_utils.PipeHandle(handle)
        self._free_instances.add(pipe)
        return pipe


class MockServer:
    def __init__(self):
        self.tasks = {}
        self.fingerprints = {}
        self.connections = set()
        self.instance_id = str(uuid.uuid4())

    def emit(self, task):
        event = {"version": 1, "type": "event", "method": "login.event",
                 "taskId": task["taskId"], "seq": task["seq"],
                 "data": {k: v for k, v in task.items() if k not in {"taskId", "seq"}}}
        for connection in tuple(self.connections):
            connection.transport.write(encode(event))

    async def finish(self, task_id, params):
        for state, stage in [("running", "authenticating"), ("succeeded", "entering_world")]:
            await asyncio.sleep(0.3)
            task = self.tasks[task_id]
            if task["state"] == "cancelled":
                return
            task.update(state=state, stage=stage, seq=task["seq"] + 1)
            if state == "succeeded":
                task["result"] = {k: params[k] for k in ("accountRef", "serverId", "characterId")}
                task["result"]["gamePid"] = 12345  # Synthetic, never a real game process.
            self.emit(task)

    def handle(self, message):
        method, params = message["method"], message["params"]
        if method == "hello":
            return {"version": 1, "instanceId": self.instance_id, "maxConcurrent": 1,
                    "methods": ["login.start", "login.switch", "login.status", "login.cancel", "ping"]}
        if method == "ping":
            return {"alive": True}
        task_id = params.get("taskId")
        if method in {"login.start", "login.switch"}:
            fingerprint = json.dumps(params, sort_keys=True)
            if task_id in self.tasks:
                if fingerprint != self.fingerprints[task_id]:
                    raise ValueError("TASK_CONFLICT")
            else:
                self.fingerprints[task_id] = fingerprint
                self.tasks[task_id] = {"taskId": task_id, "seq": 1, "state": "queued", "stage": "launching"}
                asyncio.create_task(self.finish(task_id, params))
            return self.tasks[task_id]
        if method in {"login.status", "login.cancel"}:
            if task_id not in self.tasks:
                raise ValueError("TASK_NOT_FOUND")
            task = self.tasks[task_id]
            if method == "login.cancel" and task["state"] not in {"succeeded", "failed", "cancelled"}:
                task.update(state="cancelled", seq=task["seq"] + 1)
                self.emit(task)
            return task
        raise ValueError("METHOD_NOT_FOUND")


class Peer(asyncio.Protocol):
    def __init__(self, server):
        self.server = server
        self.decoder = Decoder()
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        self.server.connections.add(self)

    def connection_lost(self, _):
        self.server.connections.discard(self)

    def data_received(self, data):
        try:
            for message in self.decoder.feed(data):
                # Fault injection is available ONLY in this standalone mock.
                if message["method"] == "test.disconnect":
                    self.transport.abort()
                    return
                if message["method"] == "test.silent":
                    continue
                reply = {"version": 1, "type": "response", "requestId": message["requestId"]}
                try:
                    reply.update(ok=True, result=self.server.handle(message))
                except ValueError as error:
                    reply.update(ok=False, error={"code": str(error), "message": str(error)})
                frame = encode(reply)
                # Deliberately split the framing header and body into separate writes.
                self.transport.write(frame[:2])
                self.transport.write(frame[2:])
        except Exception:
            self.transport.abort()


async def main(pipe):
    windows_events.PipeServer = _BytePipeServer
    state = MockServer()
    servers = await asyncio.get_running_loop().start_serving_pipe(lambda: Peer(state), pipe)
    print("READY", flush=True)
    try:
        await asyncio.Future()
    finally:
        for server in servers:
            server.close()
        for peer in tuple(state.connections):
            peer.transport.abort()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else PIPE_NAME + "-mock"))
