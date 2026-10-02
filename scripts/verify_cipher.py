"""Verify one send transformation and save a local, sequence-anchored cipher snapshot.
The snapshot is private session material: keep artifacts/ local and out of releases.
"""
import sys,pathlib,json,re,struct,subprocess,time,hashlib
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from verify_live import ROOT,rpc,text,pcap_payloads
cfg=json.loads(pathlib.Path(r'E:\tool\x64dbg2.472\release\x64\mcp_config.json').read_text())
pre='0x1492021CC';post='0x1492021CF';folder=ROOT/'artifacts'/'field-analysis'
if 'isRunning: true' not in text(rpc(cfg,'GetDebugState')):raise RuntimeError('Preserving existing pause')
existing=text(rpc(cfg,'ListBreakpoints')).lower()
if any(a[2:].lower() in existing for a in (pre,post)):raise RuntimeError('Preserving existing breakpoint')
installed=[];paused=False;proof={};capture=None
def snapshot(address):
    result=rpc(cfg,'WaitForPause',timeoutMs=3000)
    if 'PAUSED' not in text(result):raise RuntimeError('No breakpoint hit')
    regs=rpc(cfg,'GetAllRegisters');v=dict(re.findall(r'^(\w+): (0x[0-9A-Fa-f]+)',text(regs),re.M))
    if int(v['rip'],16)!=int(address,16):raise RuntimeError('Unrelated pause; preserving it')
    return v
def dump(address,length,path):
    rpc(cfg,'DumpMemory',address=address,size=length,filePath=str(path))
    data=path.read_bytes()[:length];path.write_bytes(data);return data
log=(folder/'cipher-capture.log').open('w')
try:
    capture=subprocess.Popen([str(ROOT/'build/Release/capture_probe.exe'),'Aion2.exe','12',str(folder/'cipher-session.pcap')],stdout=log,stderr=subprocess.STDOUT)
    time.sleep(.5)
    rpc(cfg,'SetBreakpoint',target=pre);installed.append(pre)
    regs=snapshot(pre);paused=True;length=int(regs['r8'],16);address=regs['rdx']
    if not 0<length<=4096:raise RuntimeError('Unexpected body size')
    state=dump(regs['rcx'],292,folder/'cipher-state-object.bin')
    plain=dump(address,length,folder/'cipher-plain.bin')
    rpc(cfg,'SetBreakpoint',target=post);installed.append(post)
    rpc(cfg,'DeleteBreakpoint',target=pre);installed.remove(pre)
    rpc(cfg,'run',timeoutMs=100);paused=False
    snapshot(post);paused=True
    cipher=dump(address,length,folder/'cipher-wire.bin')
finally:
    for a in installed:rpc(cfg,'DeleteBreakpoint',target=a)
    if paused:rpc(cfg,'run',timeoutMs=100)
    if capture:capture.wait(timeout=20)
    log.close()
i,j=struct.unpack_from('<II',state,28);table=list(state[36:292]);original=table.copy();out=bytearray()
if sorted(table)!=list(range(256)):raise RuntimeError('Not a permutation')
for byte in plain:
    old=i;i=(i+1)&255;x=table[old];j=(j+x)&255;y=table[j]
    table[old],table[j]=y,x;out.append(byte^table[(x+y)&255])
if bytes(out)!=cipher:raise RuntimeError('Transform mismatch')
matches=[]
for p in pcap_payloads(folder/'cipher-session.pcap'):
    at=p['payload'].find(cipher)
    if at>=0:
        prefix=p['payload'][:at];value=sum((v&127)<<(7*k) for k,v in enumerate(prefix))
        if not 1<=at<=4 or any(not v&128 for v in prefix[:-1]) or prefix[-1]&128 or value-4!=length:raise RuntimeError('Cannot establish frame anchor')
        matches.append({'source_ip':p['source_ip'],'dest_ip':p['dest_ip'],'source_port':p['source_port'],'dest_port':p['dest_port'],'sequence':(p['sequence']+at)&0xffffffff,'frame_sequence':p['sequence'],'time_us':p['time_us'],'payload_offset':at})
if len(matches)!=1:raise RuntimeError('Expected one unique wire match')
session={'format':'aion2-cipher-state-v1','algorithm':'observed PRGA at RVA 0x932C250','i':struct.unpack_from('<I',state,28)[0],'j':struct.unpack_from('<I',state,32)[0],'permutation':original,'anchor':matches[0]}
(folder/'cipher-session-state.json').write_text(json.dumps(session,indent=2),encoding='utf8')
anchor=matches[0]
binary=b'A2CS\x01'+bytes((session['i'],session['j']))+struct.pack('<HHI',anchor['source_port'],anchor['dest_port'],anchor['frame_sequence'])+bytes(map(int,anchor['source_ip'].split('.')))+bytes(map(int,anchor['dest_ip'].split('.')))+bytes(original)
(folder/'cipher-session.a2cs').write_bytes(binary)
proof={'body_length':length,'plain_opcode':hex(int.from_bytes(plain[:2],'little')),'transform_matches':True,'wire_match':matches[0],'plain_sha256':hashlib.sha256(plain).hexdigest(),'cipher_sha256':hashlib.sha256(cipher).hexdigest(),'final_state':text(rpc(cfg,'GetDebugState')),'final_breakpoints':text(rpc(cfg,'ListBreakpoints'))}
(folder/'cipher-proof.json').write_text(json.dumps(proof,indent=2),encoding='utf8');print(json.dumps(proof,indent=2))
