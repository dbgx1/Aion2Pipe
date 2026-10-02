import asyncio
import json
from pathlib import Path
import struct
import sys
import unittest
import uuid

from login_bridge import LoginBridge
from login_protocol import Decoder, ProtocolError, RemoteError, encode


class FramingTests(unittest.TestCase):
    def test_fragmentation_and_coalescing(self):
        message = {"version": 1, "type": "request", "requestId": "r1", "method": "ping", "params": {"text": "中文"}}
        frame = encode(message)
        decoder = Decoder()
        output = []
        for byte in frame:
            output.extend(decoder.feed(bytes([byte])))
        self.assertEqual(output, [message])
        self.assertEqual(decoder.feed(frame + frame), [message, message])

    def test_reject_bad_frames(self):
        for size in (0, 65537, 0xffffffff):
            with self.assertRaises(ProtocolError):
                Decoder().feed(struct.pack("<I", size))
        for raw in (b'\xff', b'{"version":1,"version":1}', b'NaN', b'[]'):
            with self.assertRaises(ProtocolError):
                Decoder().feed(struct.pack("<I", len(raw)) + raw)


@unittest.skipUnless(sys.platform == "win32", "Real Windows named pipes required")
class PipeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.pipe = "\\\\.\\pipe\\aion2-login-test-" + uuid.uuid4().hex
        self.process = await asyncio.create_subprocess_exec(sys.executable,
            str(Path(__file__).with_name("mock_login_server.py")), self.pipe,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        self.addAsyncCleanup(self.stop_process)
        line = await asyncio.wait_for(self.process.stdout.readline(), 5)
        self.assertEqual(line.strip(), b"READY")
        self.client = LoginBridge(self.pipe, request_timeout=0.5)
        self.addAsyncCleanup(self.client.close)
        await self.client.connect()

    async def stop_process(self):
        if self.process.returncode is None:
            self.process.terminate()
        await self.process.communicate()

    async def start(self, task_id=None, **extra):
        return await self.client.start(task_id=task_id or str(uuid.uuid4()), account_ref="account-1",
            server_id="1001", character_id="char-1", **extra)

    async def test_login_events_and_concurrent_responses(self):
        results = await asyncio.gather(*(self.client.ping() for _ in range(10)))
        self.assertTrue(all(result["alive"] for result in results))
        task = await self.start()
        self.assertEqual(task["state"], "queued")
        result = await self.client.wait(task["taskId"], timeout=3, poll_interval=0.05)
        self.assertEqual(result["state"], "succeeded")
        self.assertEqual(result["result"]["characterId"], "char-1")
        states = []
        while not self.client.events.empty():
            states.append(self.client.events.get_nowait()["state"])
        self.assertIn("running", states)

    async def test_duplicate_conflict_and_cancel(self):
        first = await self.start()
        again = await self.start(first["taskId"])
        self.assertEqual(first, again)
        with self.assertRaises(RemoteError) as caught:
            await self.start(first["taskId"], timeout_seconds=200)
        self.assertEqual(caught.exception.code, "TASK_CONFLICT")
        cancelled = await self.client.cancel(first["taskId"])
        self.assertEqual(cancelled["state"], "cancelled")
        self.assertEqual(await self.client.cancel(first["taskId"]), cancelled)

    async def test_disconnect_reconnect_query_no_restart(self):
        task = await self.start()
        with self.assertRaises(ConnectionError):
            await self.client.request("test.disconnect")
        result = await self.client.wait(task["taskId"], timeout=3, poll_interval=0.05)
        self.assertEqual(result["state"], "succeeded")
        self.assertEqual(result["seq"], 3)

    async def test_timeout_does_not_cancel_or_replay(self):
        task = await self.start()
        with self.assertRaises(TimeoutError):
            await self.client.request("test.silent")
        self.assertEqual((await self.client.wait(task["taskId"], timeout=3, poll_interval=0.05))["state"], "succeeded")
        self.assertEqual(self.client._connection.pending, {})

    async def test_unknown_task(self):
        with self.assertRaises(RemoteError) as caught:
            await self.client.status("missing")
        self.assertEqual(caught.exception.code, "TASK_NOT_FOUND")

    async def test_large_frame_and_wait_timeout(self):
        self.assertTrue((await self.client.request("ping", {"padding": "测" * 12000}))["alive"])
        task = await self.start()
        with self.assertRaises(TimeoutError):
            await self.client.wait(task["taskId"], timeout=0.02, poll_interval=0.05)
        self.assertEqual((await self.client.wait(task["taskId"], timeout=3, poll_interval=0.05))["state"], "succeeded")

    async def test_stale_events_and_bounded_queue(self):
        self.client._remember({"taskId": "test", "seq": 2, "state": "running"})
        self.client._remember({"taskId": "test", "seq": 1, "state": "queued"})
        self.assertEqual(self.client.snapshots["test"]["state"], "running")
        for seq in range(3, 270):
            self.client._remember({"taskId": "test", "seq": seq, "state": "running"})
        self.assertEqual(self.client.events.qsize(), 256)
        self.assertGreater(self.client.dropped_events, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
