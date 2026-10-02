"""Summarize the small-frame hypothesis on this project's raw-IP PCAP samples."""
import collections, json, pathlib, sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from verify_live import pcap_payloads

path=pathlib.Path(sys.argv[1])
packets=pcap_payloads(path)
streams={}
for p in packets:
    key=(p['source_port'],p['dest_port'])
    state=streams.setdefault(key,{'next':p['sequence'],'bytes':bytearray(),'gaps':0,'retransmitted':0})
    seq=p['sequence']; data=p['payload']; delta=(seq-state['next']+2**31)%2**32-2**31
    if delta>0:
        state['gaps']+=1
        continue
    if delta<0:
        skip=min(-delta,len(data)); state['retransmitted']+=skip; data=data[skip:]
    state['bytes'].extend(data); state['next']=(state['next']+len(data))%2**32
result={'sample':path.name,'payload_packets':len(packets),'hypothesis':'u8 prefix - 3; not a general Aion2 format','streams':[]}
for key,state in streams.items():
    data=state['bytes']; offset=0; sizes=collections.Counter()
    while offset<len(data):
        length=data[offset]-3
        if length<1 or offset+length>len(data): break
        sizes[length]+=1; offset+=length
    result['streams'].append({'source_port':key[0],'dest_port':key[1],'bytes':len(data),'contiguous_prefix_consumed':offset,'frames':sum(sizes.values()),'observed_forward_gaps':state['gaps'],'retransmitted_bytes':state['retransmitted'],'length_histogram':dict(sizes)})
print(json.dumps(result,indent=2))
