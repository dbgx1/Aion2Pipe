import sys, types, importlib.util, unittest, json, asyncio, tempfile, time
from pathlib import Path
from unittest.mock import patch
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(tempfile.gettempdir()) / 'aion2web-load-python'))
host = types.ModuleType('mitmproxy'); host.ctx = types.SimpleNamespace(); host.http = types.SimpleNamespace(HTTPFlow=object)
utils = types.ModuleType('mitmproxy.utils'); utils.asyncio_utils = types.SimpleNamespace()
sys.modules.update({'mitmproxy': host, 'mitmproxy.utils': utils})
spec = importlib.util.spec_from_file_location('faction_relay', Path(__file__).with_name('mitm_ws_message_monitor.py'))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class Tests(unittest.TestCase):
    def setUp(self):
        m.runtime_state = m.RuntimeState()
        m.runtime_state.update(serverKey='2201', characterId='self', userName='current', optional={'Race':'Dark'}, authorization='Bearer test', chatApiOrigin='https://lime-arizona-p4-api.plaync.com')

    def test_body_and_identity(self):
        class Response:
            status=200
            def __enter__(self): return self
            def __exit__(self,*args): pass
            def read(self): return b'{}'
        with patch.object(m.urllib.request, 'urlopen', return_value=Response()) as send:
            self.assertTrue(m._send_faction_http('hello', '2201')['ok'])
            request = send.call_args.args[0]
            self.assertEqual(request.full_url, 'https://lime-arizona-p4-api.plaync.com/gameClient/sendMessage')
            body=json.loads(request.data)
            self.assertEqual(body['gameRoomKeyInfo'], {'type':'WORLD','serverKey':'2201','roomKey':'2201'})
            self.assertNotIn('gameUserKey', body)
            self.assertEqual(body['gameMessageInfo']['userName'], 'current')
            self.assertEqual(json.loads(body['gameMessageInfo']['optional']), {'Race':'Dark'})
            self.assertNotIn('request', m._send_faction_http('hello', '2201'))

    def test_reject_stale_server_and_missing_identity(self):
        with patch.object(m.urllib.request, 'urlopen') as send:
            with self.assertRaises(ValueError): m._send_faction_http('hello','2202')
            m.runtime_state.update(characterId='other')
            with self.assertRaises(ValueError): m._send_faction_http('hello','2201')
            send.assert_not_called()

    def test_allowlist(self):
        self.assertIsNone(m._chat_api_origin('https://lime-arizona-p4-api.plaync.com.evil.test/gameClient/sendMessage'))

    def test_accelerator_ip_route_uses_tls_sni(self):
        request = types.SimpleNamespace(
            host='192.168.1.34', pretty_host='192.168.1.34', headers={},
            pretty_url='https://192.168.1.34:59377/stomp', path='/stomp')
        flow = types.SimpleNamespace(
            request=request,
            client_conn=types.SimpleNamespace(sni='lime-arizona-p4-pub.plaync.com'),
            server_conn=types.SimpleNamespace(sni=None))
        self.assertEqual(m._flow_plaync_host(flow), 'lime-arizona-p4-pub.plaync.com')
        self.assertEqual(m._effective_flow_url(flow), 'https://lime-arizona-p4-pub.plaync.com/stomp')
        self.assertTrue(m.WsMessageMonitor()._is_target_ws(flow))

    def test_outgoing_http_request_is_not_published_as_duplicate_chat(self):
        body = {'gameMessageInfo': {'userName': 'me', 'content': 'hello'}}
        request = types.SimpleNamespace(
            host='192.168.1.34', pretty_host='192.168.1.34',
            headers={}, pretty_url='https://192.168.1.34:59377/gameClient/sendMessage',
            path='/gameClient/sendMessage', method='POST',
            raw_content=json.dumps(body).encode())
        flow = types.SimpleNamespace(
            request=request,
            client_conn=types.SimpleNamespace(sni='lime-arizona-p4-api.plaync.com'),
            server_conn=types.SimpleNamespace(sni=None))
        with patch.object(m.relay, 'publish_chat') as publish:
            m.WsMessageMonitor()._print_chat_api_request(flow)
            publish.assert_not_called()

    def test_queue_deduplicates_and_expiry(self):
        async def run():
            with tempfile.TemporaryDirectory() as folder:
                relay=m.MqttRelay(Path(folder)/'relay.db')
                relay.account_switch=lambda: types.SimpleNamespace(chat_blocked=lambda:False)
                records=[]
                async def publish(record): records.append(record)
                relay._publish_record=publish
                try:
                    with patch.object(m, '_send_faction_http', return_value={'ok':True,'status':200}) as send:
                        payload={'type':'sendFactionMessage','requestId':'one','content':'hello','serverKey':'2201'}
                        relay.loop = asyncio.get_running_loop()
                        # Exercise the actual MQTT ingress, not just the downstream handler.
                        message = types.SimpleNamespace(payload=json.dumps(payload).encode(), topic=relay.control_topic, retain=False)
                        relay._on_mqtt_message(None, None, message)
                        for _ in range(100):
                            if any(record.get('type') == 'control_result' for record in records): break
                            await asyncio.sleep(.01)
                        self.assertTrue(any(record.get('type') == 'control_result' for record in records), 'MQTT ingress dropped faction command')
                        await relay._handle_control_message(json.dumps(payload))
                        self.assertEqual(send.call_count,1)
                        self.assertEqual(records[-1]['command'],'sendFactionMessage')
                        self.assertEqual(records[-1]['status'],'confirmed')
                        await relay._handle_control_message(json.dumps({**payload,'requestId':'two','expiresAt':1}))
                        self.assertEqual(send.call_count,1)
                        self.assertEqual(records[-1]['status'],'not_sent')
                finally:
                    relay._executor.shutdown(wait=True)
                    if relay._db: relay._db.close()
        asyncio.run(run())

if __name__ == '__main__':
    unittest.main()
