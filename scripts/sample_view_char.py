"""Observe one normal ViewChar action; remove only our temporary breakpoints."""
import json,pathlib,re,struct,subprocess,time,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from verify_live import rpc,text,ROOT

out=ROOT/'artifacts'/('view-char-live-'+time.strftime('%Y%m%d-%H%M%S'))
out.mkdir()
cfg=json.loads(pathlib.Path(r'E:\tool\x64dbg2.472\release\x64\mcp_config.json').read_text())
addresses=[0x149090950,0x1490909DF]
installed=[];paused=False;records=[]
def read(address,size):
    raw=text(rpc(cfg,'ReadMemory',address=hex(address),size=size));data=bytearray()
    for line in raw.splitlines():
        match=re.match(r'^\s*[0-9a-fA-F]+:\s*((?:[0-9a-fA-F]{2}(?: |$)){1,16})',line)
        if match:data.extend(bytes.fromhex(match[1]))
    if len(data)!=size:raise RuntimeError('Incomplete memory read')
    return bytes(data)
initial=text(rpc(cfg,'GetDebugState'))
if 'isRunning: true' not in initial:raise RuntimeError('Preserving existing pause')
existing=text(rpc(cfg,'ListBreakpoints')).lower()
if any(hex(a)[2:] in existing for a in addresses):raise RuntimeError('Preserving existing breakpoints')
# Refuse stale addresses before installing execution breakpoints on another build.
try:
    rpc(cfg,'PauseDebug');paused=True
    with pathlib.Path('E:/project/idb/aion2/2026_9_24/Aion2_dump_64_ida_fast.exe').open('rb') as dump:
        for address in addresses:
            dump.seek(address-0x140000000)
            if read(address,16)!=dump.read(16):raise RuntimeError('Live code differs from verified build')
finally:
    if paused:rpc(cfg,'run',timeoutMs=100);paused=False
log=(out/'capture.log').open('w',encoding='utf-8')
capture=None
try:
    for a in addresses:
        rpc(cfg,'SetBreakpoint',target=hex(a));installed.append(a)
    capture=subprocess.Popen([str(ROOT/'build/Release/capture_probe.exe'),'Aion2.exe','120',str(out/'view-char.pcap')],stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    print('READY '+str(out),flush=True)
    deadline=time.monotonic()+115
    while time.monotonic()<deadline and installed:
        state=text(rpc(cfg,'GetDebugState'))
        if 'isDebugging: false' in state:break
        if 'isRunning: false' not in state:time.sleep(.25);continue
        raw=rpc(cfg,'GetAllRegisters')
        regs={k.lower():int(v,16) for k,v in re.findall(r'^(\w+): (0x[0-9A-Fa-f]+)',text(raw),re.M)}
        at=regs.get('rip')
        if at not in installed:
            records.append({'unrelated_pause':hex(at or 0)});break
        paused=True
        record={'address':hex(at),'time_ns':time.time_ns()}
        if at==addresses[0]:
            record.update(server_id=regs['rcx']&65535,character_dbid=str(regs['rdx']),call_stack=rpc(cfg,'GetCallStack'))
        else:
            obj=read(regs['rbx'],0x38)
            base=struct.unpack_from('<Q',obj,0x18)[0];used=struct.unpack_from('<Q',obj,0x30)[0]
            if not 0<used<=128:raise RuntimeError('Unexpected query serialization size')
            record.update(serialized_size=used,serialized_hex=read(base,used).hex())
        records.append(record)
        rpc(cfg,'DeleteBreakpoint',target=hex(at));installed.remove(at)
        rpc(cfg,'run',timeoutMs=100);paused=False
        print('CAPTURED '+hex(at),flush=True)
finally:
    for a in installed:rpc(cfg,'DeleteBreakpoint',target=hex(a))
    if paused:rpc(cfg,'run',timeoutMs=100)
    (out/'evidence.json').write_text(json.dumps({'initial_state':initial,'records':records},ensure_ascii=False,indent=2),encoding='utf-8')
    print('BREAKPOINTS_REMOVED '+str(out),flush=True)
    if capture:capture.wait(timeout=140)
    log.close()
print('DONE',flush=True)
