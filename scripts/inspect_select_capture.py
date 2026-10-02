"""Offline research on the recorded IPv4 world stream; no game interaction."""
import hashlib, json, pathlib, struct, sys, zlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verify_live import pcap_payloads, ROOT

def var(data, pos):
    value = 0
    for i in range(5):
        c = data[pos]; pos += 1; value |= (c & 127) << (7*i)
        if not c & 128: return value, pos
    raise ValueError('overlong varint')

def lz4(data, expected):
    if expected > 8*1024*1024: raise ValueError('expanded size cap')
    out = bytearray(); pos = 0
    def length(base):
        nonlocal pos
        if base == 15:
            while True:
                c = data[pos]; pos += 1; base += c
                if c != 255: break
        return base
    while pos < len(data):
        token = data[pos]; pos += 1; n = length(token >> 4)
        if pos+n > len(data) or len(out)+n > expected: raise ValueError('literal bounds')
        out.extend(data[pos:pos+n]); pos += n
        if pos == len(data): break
        distance = int.from_bytes(data[pos:pos+2], 'little'); pos += 2
        n = length(token & 15)+4
        if not 0 < distance <= len(out) or len(out)+n > expected: raise ValueError('match bounds')
        for _ in range(n): out.append(out[-distance])
    if len(out) != expected: raise ValueError('expanded length mismatch')
    return bytes(out)

def frames(data, depth=0):
    if depth > 4: raise ValueError('nesting cap')
    pos = 0
    while pos < len(data):
        start = pos; n, pos = var(data, pos); end = pos+n-4
        if n < 6 or end > len(data): raise ValueError('frame bounds')
        opcode = int.from_bytes(data[pos:pos+2], 'little')
        if opcode == 0xffff:
            expanded = lz4(data[pos+6:end], int.from_bytes(data[pos+2:pos+6], 'little'))
            yield from frames(expanded, depth+1)
        else: yield opcode, data[start:end], pos-start
        pos = end

def main():
    capture = ROOT/'artifacts/movement-session-2.pcap'
    groups = {}
    for p in pcap_payloads(capture):
        if p['source_port'] == 13328 and p['payload']:
            key = (p['source_ip'],p['dest_ip'],p['source_port'],p['dest_port'])
            groups.setdefault(key, []).append(p)
    evidence = {'capture':str(capture),'capture_sha256':hashlib.sha256(capture.read_bytes()).hexdigest(),'frames':[]}
    for packets in groups.values():
        base = packets[0]['sequence']; data = bytearray()
        for p in sorted(packets,key=lambda p:(p['sequence']-base)%2**32):
            at = (p['sequence']-base)%2**32; payload = p['payload']
            if at > len(data): raise ValueError('TCP gap')
            overlap = min(len(payload),len(data)-at)
            if data[at:at+overlap] != payload[:overlap]: raise ValueError('conflicting retransmission')
            data.extend(payload[overlap:])
        for opcode, frame, prefix in frames(data):
            if opcode != 0x3620: continue
            (ROOT/'artifacts/field-analysis/select-character-frame.bin').write_bytes(frame)
            pos = prefix+4
            entity, pos = var(frame,pos); pos += 12
            auxiliary, pos = var(frame,pos); n,pos = var(frame,pos)
            blob = frame[pos:pos+n]
            item = {'opcode':'0x3620','frame_length':len(frame),'data_a_offset':pos,'data_a_length':n,'auxiliary':auxiliary}
            # A stored length before a zlib stream is a hypothesis until validated here.
            if len(blob) < 6: raise ValueError('blob too short')
            expected = struct.unpack_from('<I',blob)[0]
            if expected > 8*1024*1024: raise ValueError('inflate cap')
            obj = zlib.decompressobj(); expanded = obj.decompress(blob[4:],expected+1)
            item.update({'expanded_length_prefix':expected,'zlib_eof':obj.eof,'unused_data':len(obj.unused_data),'expanded_bytes':len(expanded),'exact_size':len(expanded)==expected})
            if not obj.eof or obj.unused_data or obj.unconsumed_tail or len(expanded)!=expected: raise ValueError('not an exact zlib envelope')
            dest = ROOT/'artifacts/field-analysis/select-character-expanded.bin';dest.write_bytes(expanded)
            item['expanded_sha256'] = hashlib.sha256(expanded).hexdigest()
            try:
                parsed = json.loads(expanded.decode('utf-16le').removesuffix('\x00'))
                item['json_type'] = type(parsed).__name__
                item['json_keys'] = list(parsed) if isinstance(parsed,dict) else []
            except (ValueError,UnicodeError): item['json_type'] = None
            evidence['frames'].append(item)
    if not evidence['frames']: raise ValueError('no select-character frame')
    (ROOT/'artifacts/field-analysis/select-character-envelope.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(evidence,ensure_ascii=False,indent=2))

if __name__ == '__main__': main()
