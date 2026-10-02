"""Take one bounded, self-cleaning x64dbg snapshot at an observed code address."""
import argparse,json,pathlib,re,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from verify_live import rpc,text,ROOT

p=argparse.ArgumentParser();p.add_argument('address');p.add_argument('name');p.add_argument('--expr',action='append',default=[]);p.add_argument('--dump',action='append',default=[]);p.add_argument('--count',type=int,default=1);args=p.parse_args()
cfg=json.loads(pathlib.Path(r'E:\tool\x64dbg2.472\release\x64\mcp_config.json').read_text())
initial=text(rpc(cfg,'GetDebugState'))
if 'isRunning: true' not in initial: raise RuntimeError('Session must be running; preserving existing pause')
if args.address.lower().removeprefix('0x') in text(rpc(cfg,'ListBreakpoints')).lower(): raise RuntimeError('Preserving existing breakpoint')
out=ROOT/'artifacts'/'field-analysis';out.mkdir(exist_ok=True)
paused=False;records=[]
try:
    rpc(cfg,'SetBreakpoint',target=args.address)
    for n in range(args.count):
        record={'address':args.address,'pause':rpc(cfg,'WaitForPause',timeoutMs=3000)}
        if 'PAUSED' not in text(record['pause']): break
        regs=rpc(cfg,'GetAllRegisters');record['registers']=regs
        values=dict(re.findall(r'^(\w+): (0x[0-9A-Fa-f]+)',text(regs),re.M))
        if int(values['rip'],16)!=int(args.address,16): raise RuntimeError('Paused elsewhere; leave the unrelated pause intact')
        paused=True
        record['disasm']=rpc(cfg,'Disassemble',address=args.address,count=12)
        record['expressions']={expr:rpc(cfg,'EvalExpression',expression=expr) for expr in args.expr}
        records.append(record)
        for index,spec in enumerate(args.dump):
            address,size=spec.rsplit(':',1);size=int(size,0)
            path=out/f'{args.name}-{n}-{index}.bin'
            if path.exists(): raise RuntimeError('Choose a new evidence name; preserving existing snapshot')
            # Some plugin versions only accept literal addresses for DumpMemory.
            resolved=values.get(address.lower(),address)
            item={'address':address,'resolved_address':resolved,'requested_size':size,'path':str(path),'result':rpc(cfg,'DumpMemory',address=resolved,size=size,filePath=str(path))}
            record.setdefault('dumps',[]).append(item)
            item['file_verified']=path.is_file() and path.stat().st_size>=size
            if not item['file_verified']: raise RuntimeError('Debugger reported a dump but no complete file exists; do not treat as evidence')
            item['plugin_file_size']=path.stat().st_size
            path.write_bytes(path.read_bytes()[:size])
        if n+1<args.count:
            rpc(cfg,'run',timeoutMs=100); paused=False
finally:
    rpc(cfg,'DeleteBreakpoint',target=args.address)
    if paused: rpc(cfg,'run',timeoutMs=100)
    (out/f'{args.name}.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(records,ensure_ascii=False,indent=2))
