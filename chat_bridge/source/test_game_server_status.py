import importlib.util
import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('server_status_relay', Path(__file__).with_name('mitm_ws_message_monitor.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class GameServerStatusTests(unittest.TestCase):
    def test_native_identity_and_expiry(self):
        with tempfile.TemporaryDirectory() as folder:
            filename = Path(folder) / 'game.json'
            relay = module.MqttRelay(Path(folder) / 'relay.sqlite3')
            try:
                module.runtime_state.data.update(serverKey='2201', userName='old-character')
                def report(server, age=0):
                    filename.write_text(json.dumps({'serverId': server, 'updatedAt': time.time()*1000-age}))
                    return relay._status_payload('online')
                with patch.dict(os.environ, {'AION2_GAME_SERVER_STATUS': str(filename)}):
                    value = report('2305')
                    self.assertEqual(value['serverId'], '2305')
                    self.assertIsNone(value['characterName'])
                    self.assertEqual(module.runtime_state.client_identity()['serverId'], '2201', 'chat authentication must not be overwritten')
                    self.assertEqual(report('2201')['characterName'], 'old-character')
                    for server, age in [(None,0), ('2305',16000), ('2305',-6000), ('65536',0), ('invalid',0)]:
                        self.assertIsNone(report(server,age)['serverId'])
                    filename.write_text('{broken')
                    self.assertIsNone(relay._status_payload('online')['serverId'])
                with patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(relay._status_payload('online')['serverId'], '2201', 'standalone legacy bridge still works')
            finally:
                relay._executor.shutdown(wait=True)
                if relay._db is not None:
                    relay._db.close()

if __name__ == '__main__':
    unittest.main()
