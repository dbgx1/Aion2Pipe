"""No network: exercise the actual runtime methods using the source AST."""
import ast
import json
import threading
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any
import sys

source = Path(sys.argv[1])
tree = ast.parse(source.read_text(encoding='utf-8'))
names = {'RuntimeState', 'WsMessageMonitor', '_normalize_optional', '_profile_name', '_is_plaync_host', '_client_info'}
subset = ast.Module(body=[node for node in tree.body if isinstance(node,(ast.ClassDef,ast.FunctionDef)) and node.name in names],type_ignores=[])
scope = dict(json=json, threading=threading, datetime=datetime, timezone=timezone, Any=Any, http=SimpleNamespace(HTTPFlow=object))
exec(compile(subset,str(source),'exec'),scope)
RuntimeState=scope['RuntimeState']
normalize=scope['_normalize_optional']
raw='{"DBId":"285415626384861764","GameAccountId":"fixture","SenderMapId":1000,"Race":"Light"}'
for value in [None,'','  ','{}','null','[]','123','broken']:
    assert normalize(value) is None
assert normalize(raw)==raw
assert json.loads(normalize({'Race':'Light'}))=={'Race':'Light'}
state=RuntimeState()
state.update(characterId='self',serverKey='1014')
def blocked():
    try: state.build_whisper_body('target','1014','hello')
    except ValueError as error: assert 'optional' in str(error)
    else: raise AssertionError('Empty optional was sent')
blocked()
def chat(cid,server,optional):
    state.observe_chat({'jsonData':{'playNcCharId':cid,'serverId':server,'userName':cid,'optional':optional}}, {'senderCharacterId':cid,'serverId':server,'sender':cid},'S->C')
chat('other','1014',raw)
assert state.data['knownUsers']['other']['optional']==raw
blocked()
chat('self','2001',raw);blocked()
chat('self','1014',raw)
body=state.build_whisper_body('target','2001','hello')
assert body['gameMessageInfo']['optional']==raw
assert body['gameUserKey']['characterId']=='target'
assert body['gameMessageInfo']['userName']=='self'
state.update(optional='');assert state.data['optional']==raw
state.remember_message_template({'gameMessageInfo':{'optional':'','content':'old'}})
assert state.build_whisper_body('target','1014','new')['gameMessageInfo']['optional']==raw
state.update(characterId='next');blocked()
assert state.data['messageInfoTemplate']=={}
state.apply_self_profile({'characterId':'next','serverId':'1014','optional':{'Race':'Light'}})
assert json.loads(state.build_whisper_body('target','1014','hello')['gameMessageInfo']['optional'])=={'Race':'Light'}
state.update(serverKey='2001');blocked()
empty=RuntimeState()
empty.observe_chat({'jsonData':{'playNcCharId':'stranger','serverId':'1014','userName':'stranger','optional':raw}}, {},'S->C')
assert empty.data['characterId'] is None and empty.data['optional'] is None
addon=scope['WsMessageMonitor'].__new__(scope['WsMessageMonitor'])
plaync=SimpleNamespace(client_hello=SimpleNamespace(sni='lime-arizona-p4-api.plaync.com'),context=SimpleNamespace(client=None))
addon.tls_clienthello(plaync)
assert not hasattr(plaync,'ignore_connection')
for host in ('8.232.194.215','campaign.example.net','device.vendor.test',None):
    other=SimpleNamespace(client_hello=SimpleNamespace(sni=host),context=SimpleNamespace(client=None))
    addon.tls_clienthello(other)
    assert other.ignore_connection is True
print('PASS: extraction, JSON string preservation, empty rejection, own sender only, cross-server isolation, switch clearing, whisper target and template override')
