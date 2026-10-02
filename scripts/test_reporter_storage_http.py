"""Local HTTP confirmation plus SQLite write-fault recovery; no production endpoints."""
import http.server, json, pathlib, subprocess, threading
root=pathlib.Path(__file__).resolve().parents[1]
requests=[]
class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        response=json.dumps(dict(ok=True,received=len(body['characters']),written=len(body['characters']))).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(response)));self.end_headers();self.wfile.write(response)
    def log_message(self,*args): pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    result=subprocess.run([str(root/'build/Release/reporter_storage_tests.exe'),f'http://127.0.0.1:{server.server_port}'],cwd=root,capture_output=True,text=True,encoding='utf-8',timeout=55)
    print(result.stdout,end='')
    assert result.returncode==0,(result.returncode,result.stderr)
    assert len(requests)==1, f'Unexpected duplicate HTTP upload: {len(requests)}'
    assert len(requests[0]['characters'])==4
    print('PASS: exactly one local HTTP upload; four durable acknowledgements after disk-full recovery')
finally:
    server.shutdown();server.server_close();thread.join()
