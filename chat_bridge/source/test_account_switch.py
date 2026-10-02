"""Actual relay handler + SQLite + separate Windows named-pipe mock process."""
import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
import uuid

host = types.ModuleType('mitmproxy')
host.ctx = types.SimpleNamespace()
host.http = types.SimpleNamespace(HTTPFlow=object)
utils = types.ModuleType('mitmproxy.utils')
utils.asyncio_utils = types.SimpleNamespace()
sys.modules.update({'mitmproxy': host, 'mitmproxy.utils': utils})
import mitm_ws_message_monitor as addon
from account_switch import AccountSwitch
from login_bridge import LoginBridge


class SwitchTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.pipe = r'\\.\pipe\aion2-switch-test-' + uuid.uuid4().hex
        self.process = await asyncio.create_subprocess_exec(sys.executable, str(Path(__file__).with_name('mock_login_server.py')),
            self.pipe, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        self.addAsyncCleanup(self.stop_process)
        self.assertEqual((await asyncio.wait_for(self.process.stdout.readline(), 5)).strip(), b'READY')
        self.records = []
        self.relays = []
        self.relay = self.new_relay()
        self.addAsyncCleanup(self.close_relays)

    def new_relay(self):
        relay = addon.MqttRelay(Path(self.folder.name) / 'relay.sqlite3')
        async def publish(record):
            self.records.append(record)
        relay._publish_record = publish
        relay._account_switch = AccountSwitch(relay, addon.AGENT_ID, lambda: LoginBridge(self.pipe, request_timeout=.5))
        self.relays.append(relay)
        return relay

    async def close_relays(self):
        for relay in self.relays:
            await relay._account_switch.close()
            relay._executor.shutdown(wait=True)
            relay._db.close()

    async def stop_process(self):
        if self.process.returncode is None:
            self.process.terminate()
        await self.process.communicate()

    def command(self, **patch):
        import time
        return {'type': 'switchAccount', 'requestId': str(uuid.uuid4()), 'taskId': str(uuid.uuid4()),
            'accountRef': 'test', 'serverKey': '1001', 'characterId': 'char1',
            'expiresAt': int(time.time() * 1000) + 30000, **patch}

    async def send(self, payload, relay=None):
        await (relay or self.relay)._handle_control_message(json.dumps(payload))

    async def finished(self, task_id, relay=None):
        service = (relay or self.relay)._account_switch
        async with asyncio.timeout(4):
            while service.row(task_id)[1]['state'] not in {'succeeded', 'failed', 'cancelled'}:
                await asyncio.sleep(.03)
        return service.row(task_id)[1]

    async def test_full_chain_progress_and_chat_pause(self):
        payload = self.command()
        await self.send(payload)
        self.assertEqual(self.records[0]['switchTask']['state'], 'received')
        await self.send({'type': 'sendWhisper', 'requestId': 'chat1', 'characterId': 'x', 'content': 'must not send'})
        self.assertEqual(self.records[-1]['status'], 'not_sent')
        result = await self.finished(payload['taskId'])
        self.assertEqual(result['state'], 'succeeded')
        states = [r['switchTask']['state'] for r in self.records if 'switchTask' in r]
        self.assertIn('running', states)
        self.assertTrue(self.relay._account_switch.chat_blocked())
        await self.relay._account_switch.resume_chat({}, {'serverId': '1001', 'characterId': 'char1'})
        self.assertTrue(self.relay._account_switch.chat_blocked())
        await self.relay._account_switch.resume_chat({'serverKey': '1001', 'characterId': 'char1'}, {'serverId': '1001', 'characterId': 'char1'})
        self.assertFalse(self.relay._account_switch.chat_blocked())
        self.assertEqual(self.relay._event_topic({'type': 'control_progress'}), self.relay.receipt_event_topic)

    async def test_duplicate_conflict_busy_expired(self):
        payload = self.command()
        await self.send(payload); await self.send(payload)
        await self.send({**payload, 'accountRef': 'different'})
        self.assertEqual(self.records[-1]['errorCode'], 'TASK_CONFLICT')
        await self.send(self.command())
        self.assertEqual(self.records[-1]['errorCode'], 'BUSY')
        await self.send(self.command(expiresAt=1))
        self.assertEqual(self.records[-1]['errorCode'], 'EXPIRED')
        self.assertEqual((await self.finished(payload['taskId']))['loginSeq'], 3)

    async def test_restart_queries_original_task_and_latest_discovery(self):
        payload = self.command()
        await self.send(payload)
        await asyncio.sleep(.15)
        await self.relay._account_switch.close()
        fresh = self.new_relay()
        await self.send({'type': 'switchAccountStatus', 'taskId': '', 'requestId': str(uuid.uuid4())}, fresh)
        self.assertEqual(self.records[-1]['taskId'], payload['taskId'])
        result = await self.finished(payload['taskId'], fresh)
        self.assertEqual(result['state'], 'succeeded')
        self.assertEqual(result['loginSeq'], 3)

    async def test_missing_login_process_is_not_success(self):
        self.relay._account_switch.bridge_factory = lambda: LoginBridge(self.pipe + '-missing', request_timeout=.1)
        payload = self.command(); await self.send(payload)
        result = await self.finished(payload['taskId'])
        self.assertEqual(result['error']['code'], 'LOGIN_UNAVAILABLE')
        self.assertFalse(result['chatPaused'])

    async def test_cancel_returns_confirmed_cancelled(self):
        payload = self.command(); await self.send(payload)
        await asyncio.sleep(.1)
        await self.send({**payload, 'type': 'cancelSwitchAccount', 'requestId': str(uuid.uuid4())})
        self.assertEqual((await self.finished(payload['taskId']))['state'], 'cancelled')

    async def test_actual_mqtt_ingress_and_no_broadcast_or_retained(self):
        payload = self.command(target=addon.AGENT_ID)
        self.relay.loop = asyncio.get_running_loop()
        def packet(value, topic=None, retain=False):
            return types.SimpleNamespace(payload=json.dumps(value).encode(), topic=topic or self.relay.control_topic, retain=retain)
        self.relay._on_mqtt_message(None, None, packet(payload, retain=True))
        self.relay._on_mqtt_message(None, None, packet({**payload, 'target': 'all'}))
        self.relay._on_mqtt_message(None, None, packet(payload, topic=self.relay.broadcast_control_topic))
        await asyncio.sleep(.02)
        self.assertEqual(self.relay._account_switch.rows(), [])
        self.relay._on_mqtt_message(None, None, packet(payload))
        async with asyncio.timeout(2):
            while not self.relay._account_switch.row(payload['taskId']):
                await asyncio.sleep(.02)
        self.assertEqual((await self.finished(payload['taskId']))['state'], 'succeeded')


if __name__ == '__main__':
    unittest.main(verbosity=2)
