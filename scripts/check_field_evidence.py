"""Independently compare on-wire fields with a simultaneously captured client object."""
import pathlib,struct,json
root=pathlib.Path(__file__).resolve().parents[1]/'artifacts'/'field-analysis'
obj=(root/'movement-decoded-0-0.bin').read_bytes()
packet=(root/'movement-decoded-0-1.bin').read_bytes()
size=struct.unpack_from('<Q',packet,16)[0]
wire=(root/'movement-decoded-0-2.bin').read_bytes()[:size]
assert size==24 and wire[0]-3==size and wire[1:3]==b'\x1c\x37'
at=3;entity=0;shift=0
while True:
    byte=wire[at];at+=1;entity|=(byte&127)<<shift;shift+=7
    if not byte&128:break
flags=wire[at];at+=1
assert flags==4, 'This evidence fixture deliberately checks one observed optional-field variant'
xyz=struct.unpack_from('<fff',wire,at);at+=12
angles=struct.unpack_from('<HH',wire,at);at+=4
last_flag=bool(wire[at]&1);at+=1
assert at==size==struct.unpack_from('<Q',packet,48)[0]
assert entity==struct.unpack_from('<I',obj,12)[0]
assert xyz==struct.unpack_from('<fff',obj,32)
assert angles==struct.unpack_from('<HH',obj,48)
assert last_flag==bool(obj[104])
result={'opcode':'0x371C','frame_bytes':size,'entity':entity,'xyz':xyz,'raw_angles':angles,'optional_flag':last_flag,'wire_equals_client_object':True,'client_consumed_all_bytes':True}
(root/'movement-field-proof.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps(result,indent=2))
