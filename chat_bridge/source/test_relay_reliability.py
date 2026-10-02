"""Durable delivery, command idempotence, overload and expiry regressions."""
import asyncio
import importlib.util
import json
import tempfile
import subprocess
import sys
import threading
import os
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('reliable_relay', Path(__file__).with_name('mitm_ws_message_monitor.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ReliabilityTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = Path(self.folder.name) / 'relay.sqlite3'
        self.relays = []
        self.original_http = module._send_whisper_http
        self.executions = 0
        def execute(*args):
            self.executions += 1
            time.sleep(.02)
            return {'ok': True, 'status': 200}
        module._send_whisper_http = execute

    def tearDown(self):
        module._send_whisper_http = self.original_http
        for relay in self.relays:
            relay._executor.shutdown(wait=True)
            if relay._db is not None:
                relay._db.close()
                relay._db = None
        self.folder.cleanup()

    def relay(self):
        relay = module.MqttRelay(self.path)
        self.relays.append(relay)
        return relay

    def capture(self, relay):
        published = []
        def publish(topic, payload, **kwargs):
            published.append(json.loads(payload))
            return SimpleNamespace(rc=0, mid=len(published))
        relay.mqtt = SimpleNamespace(is_connected=lambda: True, publish=publish)
        return published

    def command(self, request_id='one', **extra):
        return {'type':'sendWhisper', 'requestId':request_id, 'characterId':'fixture', 'content':'test', **extra}

    def test_600_offline_events_survive_reopen_and_ack(self):
        relay = self.relay()
        for i in range(600):
            relay.publish_chat({'type':'chat_message', 'message_id':str(i), 'time':'test'})
        self.assertEqual(relay.pending_event_count(), 600)
        relay._db.close(); relay._db = None
        restored = self.relay()
        self.assertEqual(restored.pending_event_count(), 600)
        published = self.capture(restored)
        restored._publish_pending_mqtt_events()
        self.assertEqual(restored.pending_event_count(), 600, 'Publish is not broker acknowledgement')
        while restored.pending_event_count():
            for mid in list(restored._inflight):
                restored._on_mqtt_publish(None, None, mid)
            restored._publish_pending_mqtt_events()
        self.assertEqual(len(published), 600)
        self.assertEqual(len({p['message_id'] for p in published}), 600)

    def test_inflight_events_survive_process_restart(self):
        relay = self.relay(); first = self.capture(relay)
        relay.publish_chat({'type':'chat_message', 'message_id':'unacked'})
        self.assertEqual(len(first), 1)
        relay._db.close(); relay._db = None
        restored = self.relay(); second = self.capture(restored)
        restored._publish_pending_mqtt_events()
        self.assertEqual(second[0]['message_id'], 'unacked')
        restored._on_mqtt_publish(None, None, 1)
        self.assertEqual(restored.pending_event_count(), 0)

    def test_abrupt_process_exit_preserves_committed_events(self):
        code = '''
import os, sys, types, importlib.util
host=types.ModuleType('mitmproxy'); host.ctx=types.SimpleNamespace(); host.http=types.SimpleNamespace(HTTPFlow=object)
utils=types.ModuleType('mitmproxy.utils'); utils.asyncio_utils=types.SimpleNamespace()
sys.modules.update({'mitmproxy':host,'mitmproxy.utils':utils})
spec=importlib.util.spec_from_file_location('relay',sys.argv[1]); module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
relay=module.MqttRelay(sys.argv[2])
for i in range(600): relay.publish_chat({'type':'chat_message','message_id':str(i)})
os._exit(0)
'''
        subprocess.run([sys.executable, '-B', '-c', code,
            str(Path(__file__).with_name('mitm_ws_message_monitor.py')), str(self.path)], check=True, timeout=30)
        restored = self.relay()
        self.assertEqual(restored.pending_event_count(), 600)

    def test_default_identity_reopens_same_database_across_processes(self):
        code = '''
import os, sys, types, importlib.util
host=types.ModuleType('mitmproxy'); host.ctx=types.SimpleNamespace(); host.http=types.SimpleNamespace(HTTPFlow=object)
utils=types.ModuleType('mitmproxy.utils'); utils.asyncio_utils=types.SimpleNamespace()
sys.modules.update({'mitmproxy':host,'mitmproxy.utils':utils})
spec=importlib.util.spec_from_file_location('relay',sys.argv[1]); module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
relay=module.MqttRelay()
if sys.argv[2]=='write': relay.publish_chat({'type':'chat_message','message_id':'restart'})
print(module.AGENT_ID, relay.pending_event_count(), flush=True)
os._exit(0)
'''
        env = {**os.environ, 'AION2_RELAY_DATA_DIR': str(self.folder.name)}
        env.pop('AION2_AGENT_ID', None)
        args = [sys.executable, '-B', '-c', code, str(Path(__file__).with_name('mitm_ws_message_monitor.py'))]
        first = subprocess.check_output([*args, 'write'], env=env, timeout=15)
        second = subprocess.check_output([*args, 'read'], env=env, timeout=15)
        self.assertEqual(first, second)
        self.assertTrue(second.strip().endswith(b' 1'))

    def test_publish_callback_does_not_take_application_lock(self):
        relay = self.relay()
        scheduled = []
        relay.loop = SimpleNamespace(is_running=lambda: True,
            call_soon_threadsafe=lambda fn: scheduled.append(fn))
        done = threading.Event()
        def callback():
            relay._on_mqtt_publish(None, None, 1)
            done.set()
        with relay._mqtt_lock:
            worker = threading.Thread(target=callback)
            worker.start()
            returned = done.wait(1)
        worker.join(2)
        self.assertTrue(returned, 'Paho callback must return while publisher owns application lock')
        self.assertEqual(len(scheduled), 1)

    def test_duplicate_command_inflight_completed_and_after_restart(self):
        relay = self.relay()
        command = self.command()
        async def run():
            await asyncio.gather(*(relay._dispatch_whisper(command) for _ in range(50)))
            await relay._dispatch_whisper(command)
        asyncio.run(run())
        self.assertEqual(self.executions, 1)
        relay._db.close(); relay._db = None
        restored = self.relay()
        asyncio.run(restored._dispatch_whisper(command))
        self.assertEqual(self.executions, 1)
        row = restored._database().execute('SELECT result FROM commands WHERE request_id="one"').fetchone()
        self.assertTrue(json.loads(row[0])['ok'])

    def test_interrupted_command_is_unknown_not_reexecuted(self):
        relay = self.relay()
        async def interrupted():
            relay._command_slots = asyncio.Semaphore(0)
            task = asyncio.create_task(relay._dispatch_whisper(self.command()))
            await asyncio.sleep(.01)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        asyncio.run(interrupted())
        restored = self.relay()
        asyncio.run(restored._dispatch_whisper(self.command()))
        result = json.loads(restored._database().execute('SELECT result FROM commands').fetchone()[0])
        self.assertEqual(result['status'], 'unknown')
        self.assertEqual(self.executions, 0)

    def test_overload_and_expired_commands_never_execute(self):
        relay = self.relay(); relay._command_capacity = 2
        async def run():
            await asyncio.gather(*(relay._dispatch_whisper(self.command(str(i))) for i in range(10)))
            await relay._dispatch_whisper(self.command('expired', expiresAt=1))
        asyncio.run(run())
        results = [json.loads(r[0]) for r in relay._database().execute('SELECT result FROM commands')]
        self.assertEqual(self.executions, 2)
        self.assertEqual(sum(r['status']=='not_sent' for r in results), 9)

    def test_queue_deadline_and_http_uncertainty(self):
        relay = self.relay()
        async def run():
            relay._command_slots = asyncio.Semaphore(0)
            await relay._dispatch_whisper(self.command('timeout', expiresAt=time.time()*1000+20))
            relay._command_slots.release()
            module._send_whisper_http = lambda *a: {'ok':False,'status':None,'error':'connection lost'}
            await relay._dispatch_whisper(self.command('uncertain'))
        asyncio.run(run())
        outcomes = dict(relay._database().execute('SELECT request_id, result FROM commands'))
        self.assertEqual(json.loads(outcomes['timeout'])['status'],'not_sent')
        self.assertEqual(json.loads(outcomes['uncertain'])['status'],'unknown')
        self.assertEqual(self.executions,0)

    def test_progress_notifications_do_not_fill_durable_outbox(self):
        relay = self.relay()
        async def run():
            await relay._publish_record({'type':'control_ack','requestId':'progress'})
            await relay._publish_record({'type':'agent_status','status':'online'})
            await relay._publish_record({'type':'control_result','requestId':'final','ok':True})
        asyncio.run(run())
        self.assertEqual(relay.pending_event_count(),1)
        stored = json.loads(relay._database().execute('SELECT payload FROM events').fetchone()[0])
        self.assertEqual(stored['requestId'],'final')

if __name__ == '__main__':
    unittest.main(verbosity=2)
