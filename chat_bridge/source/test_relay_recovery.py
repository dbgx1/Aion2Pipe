"""Local-only integration tests: real Paho 2.1, real relay, tiny MQTT broker."""
import importlib.util
import asyncio
import json
import socket
import struct
import threading
import time
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("relay_under_test", Path(__file__).with_name("mitm_ws_message_monitor.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.025)
    raise AssertionError("condition not reached")


def exact(sock, length):
    result = b""
    while len(result) < length:
        chunk = sock.recv(length - len(result))
        if not chunk:
            raise EOFError()
        result += chunk
    return result


class Broker:
    def __init__(self, port):
        self.messages = []
        self.connections = 0
        self.respond_ping = True
        self.peers = []
        self.stopped = threading.Event()
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", port))
        self.server.listen()
        self.server.settimeout(.2)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stopped.is_set():
            try:
                peer, _ = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.peers.append(peer)
            self.connections += 1
            threading.Thread(target=self.serve, args=(peer,), daemon=True).start()

    def serve(self, peer):
        try:
            while not self.stopped.is_set():
                first = exact(peer, 1)[0]
                remaining, factor = 0, 1
                while True:
                    digit = exact(peer, 1)[0]
                    remaining += (digit & 127) * factor
                    factor *= 128
                    if not digit & 128:
                        break
                body = exact(peer, remaining)
                kind = first >> 4
                if kind == 1:
                    peer.sendall(b"\x20\x02\x00\x00")
                elif kind == 8:
                    peer.sendall(b"\x90\x03" + body[:2] + b"\x00")
                elif kind == 3:
                    size = struct.unpack("!H", body[:2])[0]
                    topic = body[2:2+size].decode()
                    offset = size + 2
                    if (first >> 1) & 3:
                        peer.sendall(b"\x40\x02" + body[offset:offset+2])
                        offset += 2
                    self.messages.append((topic, json.loads(body[offset:])))
                elif kind == 12:
                    if self.respond_ping:
                        peer.sendall(b"\xd0\x00")
                elif kind == 14:
                    return
        except (EOFError, OSError):
            pass
        finally:
            peer.close()

    def drop_connections(self):
        for peer in list(self.peers):
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            peer.close()

    def close(self):
        self.stopped.set()
        self.drop_connections()
        self.server.close()
        self.thread.join(2)


class RelayRecoveryTests(unittest.TestCase):
    def test_disk_backlog_reopens_and_drains_through_real_paho(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'relay.sqlite3'
            original = module.MqttRelay(path)
            for i in range(600):
                original.publish_chat({'type':'chat_message','message_id':f'backlog-{i}'})
            original._db.close(); original._db = None
            original._executor.shutdown(wait=True)
            broker = Broker(0)
            module.MQTT_HOST, module.MQTT_PORT = '127.0.0.1', broker.server.getsockname()[1]
            restored = module.MqttRelay(path)
            restored.start()
            try:
                wait_for(restored.ready.is_set)
                wait_for(lambda: restored.pending_event_count() == 0)
                seen = [v['message_id'] for _, v in broker.messages if v.get('type') == 'chat_message']
                self.assertEqual(len(seen), 600)
                self.assertEqual(len(set(seen)), 600)
            finally:
                restored.stop(); restored.thread.join(5); broker.close()
            self.assertFalse(restored.thread.is_alive())

    def test_successful_chat_is_not_replayed_after_dedup_rotation(self):
        relay = module.MqttRelay(':memory:')
        published = []
        def publish(topic, payload, **kwargs):
            published.append(json.loads(payload))
            return SimpleNamespace(rc=0, mid=len(published))
        relay.mqtt = SimpleNamespace(is_connected=lambda: True, publish=publish)
        asyncio.run(relay._publish_record({'type': 'chat_message', 'message_id': 'original', 'time': 'test'}))
        relay._on_mqtt_publish(None, None, 1)
        for i in range(1001):
            relay._publish_mqtt_event({'type': 'control_result', 'requestId': str(i), 'time': 'test'})
            relay._on_mqtt_publish(None, None, len(published))
        for _ in range(3):
            relay._publish_pending_mqtt_events()
        self.assertEqual(sum(p.get('message_id') == 'original' for p in published), 1)
        self.assertEqual(relay.pending_event_count(), 0)

    def test_chat_before_start_is_queued_and_replayed_once(self):
        relay = module.MqttRelay(':memory:')
        record = {'type': 'chat_message', 'message_id': 'before-start'}
        relay.publish_chat(record)
        self.assertEqual(relay.pending_event_count(), 1)
        published = []
        relay.mqtt = SimpleNamespace(is_connected=lambda: True,
            publish=lambda topic, payload, **kw: (published.append(json.loads(payload)) or SimpleNamespace(rc=0, mid=len(published))))
        relay._publish_pending_mqtt_events()
        relay._publish_pending_mqtt_events()
        self.assertEqual(len(published), 1)
        self.assertEqual(relay.pending_event_count(), 1)
        relay._on_mqtt_publish(None, None, 1)
        self.assertEqual(relay.pending_event_count(), 0)

    def test_mqtt_discovery_and_commands_without_webrtc(self):
        relay = module.MqttRelay(':memory:')
        topics = []
        relay._on_mqtt_connect(SimpleNamespace(subscribe=topics.append), None, None, 0)
        self.assertEqual(set(topics), {relay.discovery_topic, relay.control_topic, relay.broadcast_control_topic})
        self.assertTrue(relay.discovery_topic.endswith('/signal/agent/all'))
        calls = []
        relay.loop = SimpleNamespace(is_running=lambda: True, call_soon_threadsafe=lambda *args: calls.append(args))
        relay._on_mqtt_message(None, None, SimpleNamespace(payload=json.dumps({'type':'offer','target':module.AGENT_ID,'sessionId':'legacy','sdp':'ignored'}).encode()))
        self.assertEqual(calls, [])
        relay._on_mqtt_message(None, None, SimpleNamespace(payload=json.dumps({'type':'discover'}).encode()))
        self.assertEqual(calls, [(relay._publish_status, 'online')])

    def test_initial_failure_reconnect_and_offline_event_replay(self):
        reserve = socket.socket()
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
        reserve.close()
        module.MQTT_HOST, module.MQTT_PORT = "127.0.0.1", port
        relay = module.MqttRelay(':memory:')
        first_failure = threading.Event()
        original_failure = relay._on_mqtt_connect_fail
        def on_failure(client, userdata):
            first_failure.set()
            original_failure(client, userdata)
        relay._on_mqtt_connect_fail = on_failure
        broker = None
        relay.start()  # Start with the broker unreachable.
        try:
            wait_for(lambda: relay.mqtt is not None)
            self.assertTrue(first_failure.wait(10), "broker must actually fail the first connection")
            self.assertTrue(relay.thread.is_alive(), "first connection failure must not terminate relay")
            broker = Broker(port)
            wait_for(relay.ready.is_set)
            wait_for(lambda: any(v.get("status") == "online" for _, v in broker.messages))
            first_connections = broker.connections
            broker.drop_connections()
            wait_for(lambda: not relay.ready.is_set())
            record = {"type": "control_result", "requestId": "local-test-only", "time": "test", "ok": True}
            relay.publish_chat(record)
            wait_for(lambda: broker.connections > first_connections and relay.ready.is_set())
            wait_for(lambda: any(v.get("requestId") == "local-test-only" for _, v in broker.messages))
            relay.publish_chat(record)
            time.sleep(.15)
            self.assertEqual(sum(v.get("requestId") == "local-test-only" for _, v in broker.messages), 1)
            # Callback validation must not kill Paho's thread on malformed input.
            for value in (b"[]", b"null", b"\xff", b"not-json"):
                relay._on_mqtt_message(relay.mqtt, None, SimpleNamespace(payload=value))
            self.assertTrue(relay.mqtt._thread.is_alive())
            # Reproduce the user's error 16: broker stops replying to PINGREQ.
            reasons = []
            original_disconnect = relay.mqtt.on_disconnect
            def disconnected(*args):
                reasons.append(str(args[3]))
                original_disconnect(*args)
            relay.mqtt.on_disconnect = disconnected
            relay.mqtt._keepalive = 1
            broker.respond_ping = False
            wait_for(lambda: any('Keep alive timeout' in reason for reason in reasons), timeout=8)
            broker.respond_ping = True
            wait_for(relay.ready.is_set)
            relay.stop()
            relay.thread.join(5)
            self.assertFalse(relay.thread.is_alive(), "stop must finish without racing two shutdown coroutines")
            self.assertFalse(relay.ready.is_set())
        finally:
            relay.stop()
            relay.thread.join(5)
            if broker:
                broker.close()

    def test_publish_failure_does_not_poison_deduplication(self):
        relay = module.MqttRelay(':memory:')
        record = {"type": "chat_message", "message_id": "retry-me"}
        relay.mqtt = SimpleNamespace(is_connected=lambda: True, publish=lambda *a, **kw: SimpleNamespace(rc=15))
        relay._publish_mqtt_event(record)
        self.assertEqual(relay.pending_event_count(), 1)
        self.assertNotIn(relay._record_key(record), relay._mqtt_event_keys)
        relay.mqtt.publish = lambda *a, **kw: SimpleNamespace(rc=0, mid=1)
        relay._publish_pending_mqtt_events()
        self.assertEqual(relay.pending_event_count(), 1)
        relay._on_mqtt_publish(None, None, 1)
        self.assertEqual(relay.pending_event_count(), 0)
        self.assertIn(relay._record_key(record), relay._mqtt_event_keys)

    def test_disconnect_during_publish_uses_pahos_existing_queue(self):
        import paho.mqtt.client as mqtt
        relay = module.MqttRelay(':memory:')
        relay.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        # The readiness check passed, but the socket disappeared before publish.
        relay.mqtt.is_connected = lambda: True
        record = {"type": "chat_message", "message_id": "socket-race"}
        relay._publish_mqtt_event(record)
        self.assertEqual(len(relay.mqtt._out_messages), 1)
        relay._publish_pending_mqtt_events()
        relay._publish_mqtt_event(record)
        self.assertEqual(relay.pending_event_count(), 1)
        self.assertEqual(len(relay.mqtt._out_messages), 1, "Paho already owns retransmission")


if __name__ == '__main__':
    unittest.main(verbosity=2)
