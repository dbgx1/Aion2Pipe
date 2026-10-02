"""Bounded dynamic evidence collection. No patching, injection, or network transmission.
The only temporary mutation is one execution breakpoint, removed in finally.
"""
import argparse, json, pathlib, re, struct, subprocess, time, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]

def rpc(config, tool, **arguments):
    body = json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':tool,'arguments':arguments}}).encode()
    request = urllib.request.Request(f'http://127.0.0.1:{config["Port"]}/',body,{'Content-Type':'application/json','Authorization':'Bearer '+config['AuthToken']})
    with urllib.request.urlopen(request,timeout=15) as response:
        result=json.load(response)
    if result.get('error') or result.get('result',{}).get('isError'):
        raise RuntimeError(result)
    return result['result']

def text(result):
    return '\n'.join(x.get('text','') for x in result.get('content',[])).replace('\\n','\n')

def pcap_payloads(path):
    b=path.read_bytes(); pos=24; result=[]
    while pos+16<=len(b):
        sec,usec,length,original=struct.unpack_from('<IIII',b,pos); pos+=16
        raw=b[pos:pos+length]; pos+=length
        if not raw or raw[0]>>4!=4 or raw[9]!=6: continue
        ih=(raw[0]&15)*4; th=(raw[ih+12]>>4)*4
        payload=raw[ih+th:struct.unpack_from('!H',raw,2)[0]]
        if payload: result.append({'time_us':sec*1000000+usec,'source_ip':'.'.join(map(str,raw[12:16])),'dest_ip':'.'.join(map(str,raw[16:20])),'source_port':struct.unpack_from('!H',raw,ih)[0], 'dest_port':struct.unpack_from('!H',raw,ih+2)[0], 'sequence':struct.unpack_from('!I',raw,ih+4)[0], 'payload':payload})
    return result

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--config',default=r'E:\tool\x64dbg2.472\release\x64\mcp_config.json'); parser.add_argument('--address',default='0x1446573BC'); args=parser.parse_args()
    config=json.loads(pathlib.Path(args.config).read_text()); dest=ROOT/'artifacts'; dest.mkdir(exist_ok=True)
    evidence={'initial_state':rpc(config,'GetDebugState'),'breakpoint':args.address}
    if 'isRunning: true' not in text(evidence['initial_state']): raise RuntimeError('Expected an already-running session; leave existing pause untouched.')
    existing=text(rpc(config,'ListBreakpoints'))
    if args.address.lower().removeprefix('0x') in existing.lower(): raise RuntimeError('Breakpoint already exists; refusing to overwrite.')
    capture=None; installed=False; stopped_by_us=False
    try:
        logfile=(dest/'correlation-capture.log').open('w',encoding='utf8')
        capture=subprocess.Popen([str(ROOT/'build/Release/capture_probe.exe'),'Aion2.exe','5',str(dest/'correlation.pcap')],stdout=logfile,stderr=subprocess.STDOUT)
        time.sleep(.7)
        rpc(config,'SetBreakpoint',target=args.address); installed=True
        evidence['pause']=rpc(config,'WaitForPause',timeoutMs=3000)
        evidence['registers']=rpc(config,'GetAllRegisters')
        regs=dict(re.findall(r'^(\w+): (0x[0-9A-Fa-f]+)',text(evidence['registers']),re.M))
        if int(regs['rip'],16)!=int(args.address,16): raise RuntimeError('Paused elsewhere; do not inspect unrelated data.')
        stopped_by_us=True
        length=int(regs['r8'],16)
        if not 0<length<=4096: raise RuntimeError('Unexpected send length')
        evidence['length']=length
        evidence['code']=rpc(config,'ReadMemory',address='0x1446573b0',size=32)
        evidence['caller']=rpc(config,'ReadMemory',address='rsp',size=64)
        # DumpMemory uses native debugger dumping, while ReadMemory returned zeros for heap buffers in this plugin.
        evidence['dump']=rpc(config,'DumpMemory',address=regs['rdx'],size=length,filePath=str(dest/'correlation-send.bin'))
    finally:
        if installed:
            rpc(config,'DeleteBreakpoint',target=args.address)
            if stopped_by_us: evidence['resume']=rpc(config,'run',timeoutMs=100)
        if capture:
            capture.wait(timeout=12); logfile.close()
    payload=(dest/'correlation-send.bin').read_bytes()[:evidence['length']]
    packets=pcap_payloads(dest/'correlation.pcap')
    matches=[{'record':i,'time_us':p['time_us'],'source_port':p['source_port'],'dest_port':p['dest_port'],'sequence':p['sequence']} for i,p in enumerate(packets) if payload in p['payload']]
    evidence['buffer_hex']=payload.hex(' '); evidence['wire_matches']=matches
    evidence['final_state']=rpc(config,'GetDebugState'); evidence['final_breakpoints']=rpc(config,'ListBreakpoints')
    (dest/'dynamic-evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps({'length':len(payload),'wire_matches':matches,'file':str(dest/'dynamic-evidence.json')},indent=2))
    if not matches: raise RuntimeError('No exact on-wire match; evidence is inconclusive')

if __name__=='__main__': main()
