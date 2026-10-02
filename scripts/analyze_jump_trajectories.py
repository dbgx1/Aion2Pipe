import json,struct,collections,hashlib,sys
from pathlib import Path
src=Path(sys.argv[1]);data=src.read_bytes();count,incomplete=struct.unpack_from('<II',data,8);pos=16;packets=[]
fmt='<QQIIBB16sH16sHII';size=struct.calcsize(fmt)
for i in range(count):
 vals=struct.unpack_from(fmt,data,pos);pos+=size;conn,t,pid,seq,flags,bits,sa,sp,da,dp,nraw,nb=vals
 raw=data[pos:pos+nraw];pos+=nraw;b=data[pos:pos+nb];pos+=nb
 if bits&8 or not bits&2 or not bits&1 or not b or dp==13700:continue
 op=int.from_bytes(b[1:3],'little') if b[0]<128 else 0
 if op not in (0x3702,0x3703,0x3718):continue
 at=5 if len(b) in (29,42) else 4
 if len(b) not in (29,41,42):continue
 packets.append({'packet':i+1,'time':t,'conn':conn,'tool':bool(bits&4),'op':hex(op),'flags':b[3],'xyz':list(struct.unpack_from('<fff',b,at)),'yaw':struct.unpack_from('<f',b,at+12)[0],'vz':struct.unpack_from('<f',b,at+24)[0] if len(b)!=29 else 0,'client_time':struct.unpack_from('<Q',b,len(b)-8)[0],'hex':b.hex()})
assert pos==len(data)
sequences=[];current={}
for p in packets:
 key=(p['conn'],p['tool'])
 if p['op']=='0x3702' and p['flags']==20:
  if current.get(key):sequences.append(current.pop(key))
  current[key]=[]
 if key not in current:continue
 current[key].append(p)
 if p['op']=='0x3718':sequences.append(current.pop(key))
sequences.extend(current.values());sequences.sort(key=lambda g:g[0]['time'])
rows=[json.loads(x) for x in Path(sys.argv[2]).read_text('utf8').splitlines()];events=[]
def walk(r,parent=None):
 if parent:r={**r,**{k:parent[k] for k in ('time_us','proxy_connection','inbound_by_port','packet_ids','tool_generated')}}
 if r.get('name','').startswith('0x'):
  r['op']=r['name'].split()[0].lower();r['values']={f['name']:f['value'] for f in r['fields']};events.append(r)
 for child in r.get('children',[]):walk(child,r)
for r in rows:
 if r['kind']=='message':walk(r)
events.sort(key=lambda r:(r['time_us'],r['packet_ids'][0]))
results=[]
for group in sequences:
 first=group[0];last=group[-1];start=first['time'];stop=last['time']
 self_id=None
 for event in events:
  if event['time_us']>start:break
  if event['proxy_connection']==first['conn'] and event['inbound_by_port']:
   if event['op']=='0x3621':self_id=None
   elif event['op']=='0x3633' and event['structure_complete']:self_id=event['values'].get('实体编号')
 own=[r for r in events if r['proxy_connection']==first['conn'] and r['inbound_by_port'] and start<r['time_us']<=stop+600000 and self_id is not None and r['values'].get('实体编号')==self_id]
 statuses=[r for r in own if r['op']=='0x382a' and r['time_us']>=stop and any(f['name'].endswith('异常状态 ID') and f['value']=='231' for f in r['fields'])]
 result={'complete_sequence':last['op']=='0x3718','self_entity':self_id,'connection':first['conn'],'tool':first['tool'],'start_packet':first['packet'],'stop_packet':last['packet'] if last['op']=='0x3718' else None,'last_packet':last['packet'],'start_us':start,'frames':len(group),'duration_ms':round((stop-start)/1000,3),'client_duration_ms':last['client_time']-first['client_time'],'z_start':first['xyz'][2],'z_peak':max(p['xyz'][2] for p in group),'z_end':last['xyz'][2],'rise':max(p['xyz'][2] for p in group)-first['xyz'][2],'status231_after_stop_ms':[round((r['time_us']-stop)/1000,3) for r in statuses], 'own_opcodes':dict(collections.Counter(r['op'] for r in own))}
 if first['tool']:
  templates=[g for g in sequences if not g[0]['tool'] and g[0]['conn']==first['conn'] and g[-1]['time']<start and len(g)==len(group)]
  for template in reversed(templates):
   equivalent=True
   for a,b in zip(template,group):
    expected=bytearray.fromhex(a['hex']);at=5 if len(expected) in (29,42) else 4
    target=[first['xyz'][i]+a['xyz'][i]-template[0]['xyz'][i] for i in range(3)]
    if a['op']=='0x3718':target=first['xyz']
    struct.pack_into('<ffff',expected,at,*target,first['yaw']);struct.pack_into('<Q',expected,len(expected)-8,first['client_time']+a['client_time']-template[0]['client_time'])
    if expected.hex()!=b['hex']:equivalent=False;break
   if equivalent:result['byte_exact_rebased_native_template_packet']=template[0]['packet'];break
  result['trajectory']=[{'packet':p['packet'],'op':p['op'],'elapsed_ms':round((p['time']-start)/1000,3),'client_elapsed_ms':p['client_time']-first['client_time'],'z':p['xyz'][2],'vz':p['vz']} for p in group]
 results.append(result)
out={'file':str(src),'sha256':hashlib.sha256(data).hexdigest(),'incomplete':bool(incomplete),'packets':count,'streams':[r for r in rows if r['kind']=='stream'],'sequences':results}
Path(sys.argv[3]).write_text(json.dumps(out,ensure_ascii=False,indent=2),'utf8')
for r in results:print(json.dumps({k:v for k,v in r.items() if k!='trajectory'},ensure_ascii=True))
