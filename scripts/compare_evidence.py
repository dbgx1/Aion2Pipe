"""Verify the saved static/runtime code bytes without touching the debugger."""
import json, pathlib, re
root=pathlib.Path(__file__).resolve().parents[1]/'artifacts'
dynamic=json.loads((root/'dynamic-evidence.json').read_text(encoding='utf8'))
static=json.loads((root/'ida-send-bytes.json').read_text(encoding='utf8'))
expected=bytes(int(token,16) for token in static['result'][0]['data'].split())
dump=dynamic['code']['content'][0]['text'].replace('\\n','\n')
actual=bytearray()
for line in dump.splitlines():
    match=re.match(r'\s+[0-9A-F]+:\s+((?:[0-9A-F]{2} ){16})',line)
    if match: actual.extend(bytes.fromhex(match.group(1)))
assert len(actual)==32 and bytes(actual)==expected,'Static/runtime code mismatch'
assert dynamic['wire_matches'],'No buffer-to-wire match'
result={'code_bytes_equal':True,'compared_bytes':32,'send_buffer_length':dynamic['length'],'exact_wire_matches':len(dynamic['wire_matches'])}
(root/'comparison-result.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps(result,indent=2))
