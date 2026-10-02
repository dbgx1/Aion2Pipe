"""MQTT -> durable switch task -> local login IPC -> MQTT status snapshots."""
import asyncio
from datetime import datetime, timezone
import json
import os
import time

from login_bridge import LoginBridge
from login_protocol import PIPE_NAME, ProtocolError, RemoteError, TERMINAL

COMMANDS = {"switchAccount", "switchAccountStatus", "cancelSwitchAccount"}


class AccountSwitch:
    def __init__(self, relay, agent_id, bridge_factory=None):
        self.relay = relay
        self.agent_id = agent_id
        self.bridge_factory = bridge_factory or (lambda: LoginBridge(os.getenv("AION2_LOGIN_PIPE", PIPE_NAME)))
        self.monitors = {}
        self.lock = asyncio.Lock()
        with relay._mqtt_lock:
            db = relay._database()
            db.execute('CREATE TABLE IF NOT EXISTS account_switch_tasks (task_id TEXT PRIMARY KEY, params TEXT NOT NULL, snapshot TEXT NOT NULL)')
            db.commit()

    def rows(self):
        with self.relay._mqtt_lock:
            return self.relay._database().execute('SELECT task_id, params, snapshot FROM account_switch_tasks').fetchall()

    def row(self, task_id):
        with self.relay._mqtt_lock:
            row = self.relay._database().execute('SELECT params, snapshot FROM account_switch_tasks WHERE task_id=?', (task_id,)).fetchone()
        return (json.loads(row[0]), json.loads(row[1])) if row else None

    def chat_blocked(self):
        # Switching invalidates the old sender context. Chat stays paused even on
        # failure until the operator explicitly confirms the current game identity.
        return any(json.loads(row[2]).get("chatPaused", True) for row in self.rows())

    async def emit(self, snapshot, request_id=None):
        await self.relay._publish_record({"type": "control_progress", "command": "switchAccount",
            "agentId": self.agent_id, "requestId": request_id or snapshot["taskId"],
            "taskId": snapshot["taskId"], "switchTask": snapshot,
            "time": datetime.now(timezone.utc).isoformat()})

    async def update(self, task_id, **changes):
        params, snapshot = self.row(task_id)
        snapshot.update(changes)
        snapshot["revision"] += 1
        snapshot["updatedAt"] = int(time.time() * 1000)
        with self.relay._mqtt_lock:
            db = self.relay._database()
            with db:
                db.execute('UPDATE account_switch_tasks SET snapshot=? WHERE task_id=?', (json.dumps(snapshot), task_id))
        await self.emit(snapshot)
        return snapshot

    async def error(self, payload, code, message):
        await self.relay._publish_record({"type": "control_result", "command": payload.get("type"),
            "agentId": self.agent_id, "requestId": payload.get("requestId"), "taskId": payload.get("taskId"),
            "ok": False, "status": "not_sent", "error": message, "errorCode": code,
            "time": datetime.now(timezone.utc).isoformat()})

    async def handle(self, payload):
        task_id = payload.get("taskId")
        request_id = payload.get("requestId")
        if payload.get("type") == "switchAccountStatus" and not task_id:
            rows = self.rows()
            if rows:
                task_id = rows[-1][0]
                payload = {**payload, "taskId": task_id}
            else:
                return await self.error(payload, "TASK_NOT_FOUND", "该客户端尚无换号任务")
        if any(not isinstance(value, str) or not value.strip() or len(value) > 128 for value in (task_id, request_id)):
            return await self.error(payload, "INVALID_PARAMS", "缺少有效任务编号或请求编号")
        async with self.lock:
            row = self.row(task_id)
            kind = payload["type"]
            if kind == "switchAccount":
                params = {"task_id": task_id, "account_ref": payload.get("accountRef"),
                    "server_id": payload.get("serverKey"), "character_id": payload.get("characterId"),
                    "timeout_seconds": payload.get("timeoutSeconds", 180)}
                if any(not isinstance(params[k], str) or not params[k].strip() or len(params[k]) > 128
                       for k in ("account_ref", "server_id", "character_id")) or type(params["timeout_seconds"]) is not int or not 10 <= params["timeout_seconds"] <= 900:
                    return await self.error(payload, "INVALID_PARAMS", "账号引用、区服、角色或超时参数无效")
                if row:
                    if row[0] != params:
                        return await self.error(payload, "TASK_CONFLICT", "同一个任务编号不能换成另一组换号参数")
                    await self.emit(row[1], request_id)
                    self.watch(task_id, start=False)
                    return
                expires = payload.get("expiresAt")
                if type(expires) not in (int, float) or not 0 < expires - time.time() * 1000 <= 60000:
                    return await self.error(payload, "EXPIRED", "换号指令已过期或缺少有效期")
                if any(json.loads(item[2])["state"] not in TERMINAL for item in self.rows()) or self.relay._active_commands:
                    return await self.error(payload, "BUSY", "当前仍有换号或聊天指令执行中，请稍后查询")
                snapshot = {"taskId": task_id, "revision": 1, "state": "received", "stage": "",
                    "message": "客户端已收到，准备交给登录进程", "accountRef": params["account_ref"],
                    "serverId": params["server_id"], "characterId": params["character_id"],
                    "updatedAt": int(time.time() * 1000), "chatPaused": True}
                with self.relay._mqtt_lock:
                    db = self.relay._database()
                    with db:
                        db.execute('INSERT INTO account_switch_tasks VALUES (?,?,?)', (task_id, json.dumps(params), json.dumps(snapshot)))
                await self.emit(snapshot, request_id)
                self.watch(task_id, start=True)
                return
            if not row:
                return await self.error(payload, "TASK_NOT_FOUND", "客户端没有该换号任务，不会自动重新执行")
            if kind == "switchAccountStatus":
                await self.emit(row[1], request_id)
                self.watch(task_id, start=False)
            elif kind == "cancelSwitchAccount":
                try:
                    async with self.bridge_factory() as bridge:
                        await self.accept(task_id, await bridge.cancel(task_id))
                except Exception:
                    return await self.error(payload, "CANCEL_UNCONFIRMED", "取消尚未确认，请继续查询任务状态")

    def watch(self, task_id, start):
        if task_id in self.monitors or self.row(task_id)[1]["state"] in TERMINAL:
            return
        task = asyncio.create_task(self.monitor(task_id, start))
        self.monitors[task_id] = task
        task.add_done_callback(lambda _: self.monitors.pop(task_id, None))

    async def accept(self, task_id, snapshot):
        row = self.row(task_id)[1]
        if snapshot["seq"] < row.get("loginSeq", 0):
            return
        if snapshot["seq"] == row.get("loginSeq") and row["state"] != "unknown":
            return
        if row["state"] in TERMINAL:
            return
        if snapshot["state"] == "succeeded":
            result = snapshot.get("result", {})
            if any(str(result.get(k)) != str(row[k]) for k in ("accountRef", "serverId", "characterId")):
                raise ProtocolError("登录进程返回的账号或角色与任务不一致")
        await self.update(task_id, state=snapshot["state"], stage=snapshot.get("stage", ""),
            loginSeq=snapshot["seq"], message=snapshot.get("message", ""), error=snapshot.get("error"),
            result=snapshot.get("result"), cancelRequested=snapshot.get("cancelRequested", False))

    async def monitor(self, task_id, start):
        bridge = self.bridge_factory()
        started = time.monotonic()
        params, _ = self.row(task_id)
        try:
            if start:
                try:
                    await bridge.connect()
                except Exception:
                    await self.update(task_id, state="failed", chatPaused=False, error={"code": "LOGIN_UNAVAILABLE", "message": "无法连接登录进程，未发送换号请求"}, message="无法连接登录进程，未发送换号请求")
                    return
                try:
                    await self.accept(task_id, await bridge.switch_account(**params))
                except RemoteError as error:
                    await self.update(task_id, state="failed", error={"code": error.code, "message": error.message}, message=error.message)
                    return
                except Exception:
                    await self.update(task_id, state="unknown", message="登录进程未确认受理，正在查询原任务；不会重复换号")
            # A restarted client only queries. It never replays login.switch.
            while time.monotonic() - started < params["timeout_seconds"] + 60:
                if self.row(task_id)[1]["state"] in TERMINAL:
                    return
                try:
                    snapshot = await bridge.status(task_id)
                    await self.accept(task_id, snapshot)
                    if snapshot["state"] in TERMINAL:
                        return
                    try:
                        event = await asyncio.wait_for(bridge.events.get(), 1)
                        if event["taskId"] == task_id:
                            await self.accept(task_id, event)
                    except TimeoutError:
                        pass
                except asyncio.CancelledError:
                    raise
                except Exception:
                    if self.row(task_id)[1]["state"] != "unknown":
                        await self.update(task_id, state="unknown", message="与登录进程的状态未确认，保留任务编号并重试查询")
                    await asyncio.sleep(2)
            if self.row(task_id)[1]["state"] not in TERMINAL:
                await self.update(task_id, state="unknown", message="等待已超时，请查询原任务或到登录进程核对，不能重复换号")
        finally:
            await bridge.close()

    async def resume_chat(self, payload, identity):
        async with self.lock:
            await self._resume_chat(payload, identity)

    async def _resume_chat(self, payload, identity):
        # Explicit operator action, with exact current identity. Does not resume
        # any paused browser AI task automatically.
        if any(json.loads(row[2])["state"] not in TERMINAL for row in self.rows()):
            return await self.error(payload, "BUSY", "换号结果未确认，不能恢复聊天")
        if not identity.get("characterId") or str(identity.get("characterId")) != str(payload.get("characterId")) or str(identity.get("serverId")) != str(payload.get("serverKey")):
            return await self.error(payload, "IDENTITY_MISMATCH", "客户端识别到的当前角色尚未与确认信息一致")
        for task_id, _, raw in self.rows():
            if json.loads(raw).get("chatPaused"):
                await self.update(task_id, chatPaused=False)

    async def close(self):
        tasks = list(self.monitors.values())
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
