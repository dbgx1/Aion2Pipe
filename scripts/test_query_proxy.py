"""Local-only integration: real WinDivert + TCP sockets, independent OpenSSL peers.
No game interaction, debugger, DLL injection, stored session states, or external server.
"""
import concurrent.futures, json, pathlib, socket, struct, subprocess, time, sys, os, threading, shutil
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import serialization, hashes
try:
    from Crypto.Cipher import ARC4
except ImportError:
    from cryptography.hazmat.primitives.ciphers import Cipher
    from cryptography.hazmat.decrepit.ciphers.algorithms import ARC4 as ARC4Algorithm
    class ARC4:
        @staticmethod
        def new(key):
            context=Cipher(ARC4Algorithm(key),mode=None).encryptor()
            return type('ARC4Context',(),{'encrypt':lambda self,data:context.update(data),'decrypt':lambda self,data:context.update(data)})()

ROOT = pathlib.Path(__file__).resolve().parents[1]
def var(n):
    out=bytearray()
    while n>=128: out.append((n&127)|128); n>>=7
    out.append(n); return bytes(out)
def unvar(b,at=0):
    n=0
    for i in range(5):
        c=b[at];at+=1;n|=(c&127)<<(7*i)
        if not c&128:return n,at
    raise ValueError('invalid varint')
def frame(body):return var(len(body)+4)+body
def exact(s,n):
    out=bytearray()
    while len(out)<n:
        b=s.recv(n-len(out))
        if not b:raise EOFError('truncated frame')
        out.extend(b)
    return bytes(out)
def receive(s):
    prefix=bytearray()
    for _ in range(5):
        prefix.extend(exact(s,1))
        if prefix[-1]<128:break
    n,_=unvar(prefix)
    assert 6<=n<=2097152
    return exact(s,n-4)
def fragmented(s,b):
    # Headers and key blobs intentionally cross socket writes.
    for at in range(0,len(b),7):s.sendall(b[at:at+7])
def connect_proxy(listener,stop):
    def handle(client):
        upstream=None
        try:
            client.settimeout(8);header=bytearray()
            while b'\r\n\r\n' not in header and len(header)<16384:header.extend(client.recv(1024))
            first=bytes(header).split(b'\r\n',1)[0].decode('ascii');authority=first.split(' ')[1];host,port=authority.rsplit(':',1)
            upstream=socket.create_connection((host,int(port)),8);client.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
            def copy(source,target):
                try:
                    while data:=source.recv(65536):target.sendall(data)
                    target.shutdown(socket.SHUT_WR)
                except OSError:pass
            a=threading.Thread(target=copy,args=(client,upstream),daemon=True);b=threading.Thread(target=copy,args=(upstream,client),daemon=True);a.start();b.start();a.join();b.join()
        finally:
            if upstream:upstream.close()
            client.close()
    listener.settimeout(.2)
    while not stop.is_set():
        try:client,_=listener.accept()
        except TimeoutError:continue
        threading.Thread(target=handle,args=(client,),daemon=True).start()
def connect_proxy_child():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0));listener.listen(8)
        print(f'PORT={listener.getsockname()[1]}',flush=True)
        connect_proxy(listener,threading.Event())
OAEP=padding.OAEP(mgf=padding.MGF1(hashes.SHA1()),algorithm=hashes.SHA1(),label=None)
def lz4_literal(b):
    out=bytearray([min(len(b),15)<<4])
    if len(b)>=15:
        n=len(b)-15
        while n>=255:out.append(255);n-=255
        out.append(n)
    return bytes(out)+b
def main():
    relay_mode='--local-relay' in sys.argv
    upstream_mode='--upstream' in sys.argv
    relay_host='127.0.0.1' if relay_mode else '127.0.0.2'
    expected_client_port=[0]
    overflow='--overflow' in sys.argv; heartbeats=10004 if overflow else 4
    cancel_mode='--jump-cancel' in sys.argv
    penalty_mode='--jump-penalty' in sys.argv
    repeat_mode='--jump-repeat' in sys.argv
    flat_mode='--flat' in sys.argv
    ground_height_mode='--ground-height' in sys.argv
    ground_height=bytes.fromhex('463701')+struct.pack('<f',30)
    jump_mode=flat_mode or '--jump' in sys.argv or cancel_mode or penalty_mode or repeat_mode
    secret=bytes(range(32));heartbeat=bytes.fromhex('01360102030405060708');notice=bytes.fromhex('00360102030405060708')
    client_key=rsa.generate_private_key(public_exponent=3,key_size=2048)
    public=client_key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.PKCS1)
    movement=lambda x: bytes.fromhex('013702')+struct.pack('<fffHfBQ',x,20,30,32768,-15,1,1790672818300)
    native_jump=bytes.fromhex('023714')+struct.pack('<fffffffBQ',42,20,30,-15,0,0,1000,3,1790672818200)
    trajectory=json.loads((ROOT/'artifacts/field-analysis/stationary-jump-trajectory.json').read_text('utf8'))
    jump_frames=[]
    for item in trajectory:
        data=bytearray.fromhex(item['hex']);at=5 if len(data) in (29,42) else 4
        z=struct.unpack_from('<f',data,at+8)[0]
        struct.pack_into('<ffff',data,at,42,20,30+z-21914.5390625,-15)
        jump_frames.append(bytes(data[1:]))
    native_seed=jump_frames
    if flat_mode:
        # No native jump is sent: only a stopped position from this connection.
        native_seed=[bytes.fromhex('18370000')+struct.pack('<ffffQ',42,20,30,-15,1790672818300)]
        trajectory=[{'offset_ms':t} for t in (0,100,200,300,400,444,500,600,700,800,888)]
        jump_frames=[]
        f32=lambda x:struct.unpack('<f',struct.pack('<f',x))[0]
        for item in trajectory:
            ms=item['offset_ms'];t=ms/1000
            if ms==888:body=bytes.fromhex('18370000')+struct.pack('<ffffQ',42,20,30,-15,1)
            else:
                prefix=bytes.fromhex('023714') if ms==0 else bytes.fromhex('02371102') if ms==444 else bytes.fromhex('033702')
                body=prefix+struct.pack('<fffffffBQ',42,20,f32(30+f32(1000*t-1127*t*t)),-15,0,0,1000-2254*t,3 if ms==0 else 1,1)
            jump_frames.append(body)
    # Build 3526 observed in the 2026-09-30 native handshake sender.
    suffix=struct.pack('<I',3526)+bytes.fromhex('0306030000')
    response=bytes.fromhex('503600000103616263')+struct.pack('<IIIIHHBI',3001,50,5,1234,1005,1006,1,42)
    response+=struct.pack('<QBHB I Q H B',2**64-1,0,65535,0,123,0,0,0)
    with socket.socket() as listener, socket.socket() as upstream_listener:
        listener.bind((relay_host,0));listener.listen(4);listener.settimeout(12);port=listener.getsockname()[1]
        environment=os.environ.copy();proxy_stop=threading.Event();proxy_thread=None;proxy_process=None
        if upstream_mode:
            proxy_exe=pathlib.Path(sys.executable).with_name('connect-proxy-test.exe');shutil.copy2(sys.executable,proxy_exe)
            proxy_process=subprocess.Popen([str(proxy_exe),__file__,'--connect-proxy-child'],stdout=subprocess.PIPE,text=True,encoding='utf8',creationflags=subprocess.CREATE_NO_WINDOW)
            line=proxy_process.stdout.readline().strip();assert line.startswith('PORT=');environment['AION2PIPE_TEST_UPSTREAM_PORT']=line.split('=',1)[1]
        proc=subprocess.Popen([str(ROOT/'build/Release/query_proxy_probe.exe'),str(13328 if relay_mode else port)]+(['--local-relay'] if relay_mode else ['--overflow'] if overflow else ['--jump-repeat-cancel'] if repeat_mode and (cancel_mode or penalty_mode) else ['--jump-cancel'] if cancel_mode or penalty_mode else ['--jump-repeat'] if repeat_mode else ['--jump'] if jump_mode else [])+(['--flat'] if flat_mode else []),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf8',cwd=ROOT,creationflags=subprocess.CREATE_NO_WINDOW,env=environment)
        try:
            assert proc.stdout.readline().strip()=='READY','proxy startup failed'
            with socket.socket() as login_listener:
                login_listener.bind((relay_host,13700));login_listener.listen(1);login_listener.settimeout(8)
                with socket.socket() as login_client:
                    login_client.settimeout(8);login_client.bind(('127.0.0.1',0));login_client.connect((relay_host,13700))
                    with login_listener.accept()[0] as login_server:
                        login_server.settimeout(8);login_client.sendall(frame(notice));assert receive(login_server)==notice
                        login_server.sendall(frame(notice));assert receive(login_client)==notice
            if relay_mode:
                # Same local relay also carries non-game traffic; three prefix
                # bytes must be forwarded without waiting for a game frame.
                with socket.socket() as other_listener:
                    other_listener.bind((relay_host,0));other_listener.listen(1);other_listener.settimeout(8)
                    with socket.socket() as other_client:
                        other_client.settimeout(8);other_client.bind(('127.0.0.1',0));source_port=other_client.getsockname()[1]
                        other_client.connect(other_listener.getsockname());other_client.sendall(b'\x16\x03\x01')
                        other_server,other_peer=other_listener.accept()
                        with other_server:
                            other_server.settimeout(8);assert other_peer[1]==source_port
                            assert exact(other_server,3)==b'\x16\x03\x01'
                            other_server.sendall(b'\x15\x03\x03');assert exact(other_client,3)==b'\x15\x03\x03'
            def server():
                accepted,peer=listener.accept()
                with accepted as s:
                    if relay_mode:assert peer[1]==expected_client_port[0], "accelerator source port changed"
                    s.settimeout(12);rq=receive(s);assert rq[:2]==b'\x10\x36'
                    n,at=unvar(rq,2);replacement=rq[at:at+n];assert replacement!=public and rq[at+n:]==suffix
                    peer=serialization.load_der_public_key(replacement);assert peer.key_size==2048
                    encrypted=peer.encrypt(secret,OAEP)
                    fragmented(s,frame(b'\x11\x36\0\0'+b'\x01\x02\x03\x04'+var(256)+encrypted+bytes(12)))
                    s.sendall((ROOT/'artifacts/field-analysis/proxy-self-fixture.bin').read_bytes())
                    decrypt=ARC4.new(secret);seen=[]
                    while True:
                        plain=decrypt.decrypt(receive(s));seen.append(plain[:2].hex())
                        if plain[:2]==b'\x4f\x36':
                            assert plain==b'\x4f\x36'+struct.pack('<HQ',2017,123456);break
                        assert plain in (tuple([heartbeat]+native_seed) if jump_mode else (heartbeat,movement(42)))
                    assert seen.count('0136')==heartbeats,seen
                    expanded=frame(notice)+frame(response)+frame(notice)
                    fragmented(s,frame(b'\xff\xff'+struct.pack('<I',len(expanded))+lz4_literal(expanded)))
                    if jump_mode:
                        repeat_starts=[];repeat_stamps=[]
                        for cycle in range(2 if repeat_mode and not (cancel_mode or penalty_mode) else 1):
                            timings=[];stamp0=None
                            for i,expected in enumerate(jump_frames[:1] if cancel_mode or penalty_mode else jump_frames):
                                action=decrypt.decrypt(receive(s));timings.append(time.monotonic())
                                assert action[:-8]==expected[:-8],('jump body mismatch',i,action.hex(),expected.hex())
                                stamp=struct.unpack('<Q',action[-8:])[0]
                                if stamp0 is None:stamp0=stamp
                                assert stamp-stamp0==trajectory[i]['offset_ms'],'client cadence mismatch'
                                if ground_height_mode and i==1:s.sendall(frame(ground_height))
                            duration=(timings[-1]-timings[0])*1000
                            if not (cancel_mode or penalty_mode):assert 650<=duration<=1200,('sequence timing mismatch',duration)
                            repeat_starts.append(timings[0]);repeat_stamps.append(stamp0)
                        if repeat_mode and not (cancel_mode or penalty_mode):
                            assert 5900<=(repeat_starts[1]-repeat_starts[0])*1000<=6500,'repeat interval mismatch'
                            assert 5800<=repeat_stamps[1]-repeat_stamps[0]<=6500,'repeat timestamp not refreshed'
                            s.sendall(frame(notice))
                            assert decrypt.decrypt(receive(s))==heartbeat,'user stop failed: extra jump before native heartbeat'
                        if penalty_mode:
                            penalty=bytes.fromhex('1a37010c00')+bytes(12)+bytes.fromhex('000001e803000000000000')
                            s.sendall(frame(penalty))
                        else:s.sendall(frame(notice))
                        assert decrypt.decrypt(receive(s))==movement(99),'native movement desynchronized after generated jump'
                        if cancel_mode or penalty_mode:s.sendall(frame(notice))
                        assert decrypt.decrypt(receive(s))==heartbeat,'native heartbeat desynchronized after generated jump'
                    else:assert decrypt.decrypt(receive(s))==movement(99),'post-insertion cipher desynchronized'
                    assert s.recv(1)==b'','client half-close not forwarded'
                    s.sendall(frame(notice));s.shutdown(socket.SHUT_WR)
                    return {'client_exponent':3,'proxy_exponent':peer.public_numbers().e,'observed_opcodes':seen[:16],'observed_client_frames':len(seen)-1,'rsa_oaep_sha1_interop':True,'rc4_interop':True,'post_query_cipher_sync':True,'tcp_half_close':True,'jump_frames':(1 if cancel_mode or penalty_mode else len(jump_frames)) if jump_mode else 0,'jump_elapsed_ms':round(duration,2) if jump_mode else None}
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future=executor.submit(server)
                with socket.socket() as client:
                    client.settimeout(12);client.bind(('127.0.0.1',0));expected_client_port[0]=client.getsockname()[1];client.connect((relay_host,port))
                    fragmented(client,frame(b'\x10\x36'+var(len(public))+public+suffix))
                    rs=receive(client);assert rs[:2]==b'\x11\x36';n,at=unvar(rs,8);assert n==256
                    assert client_key.decrypt(rs[at:at+n],OAEP)==secret,'client handshake key mismatch'
                    assert receive(client)[:2]==bytes.fromhex('3336')
                    encrypt=ARC4.new(secret)
                    client.sendall(b''.join(frame(encrypt.encrypt(b)) for b in [heartbeat]*heartbeats+(native_seed if jump_mode else [movement(42)])))
                    assert receive(client)==notice and receive(client)==notice,'proxy query response leaked into game stream'
                    if jump_mode:
                        feedback=receive(client)
                        received_heights=0
                        while ground_height_mode and feedback==ground_height:
                            received_heights+=1;feedback=receive(client)
                        if ground_height_mode:assert received_heights==(2 if repeat_mode else 1),'ground height was not forwarded intact'
                        assert feedback[:2]==bytes.fromhex('1a37') if penalty_mode else feedback==notice
                        if penalty_mode:time.sleep(0.4)  # Penalty alone must cancel before any native movement.
                    if repeat_mode and not (cancel_mode or penalty_mode):
                        time.sleep(6.3)  # The probe stops after two jumps. No third jump may precede this heartbeat.
                        client.sendall(frame(encrypt.encrypt(heartbeat)))
                        assert receive(client)==notice
                    client.sendall(frame(encrypt.encrypt(movement(99))))
                    if cancel_mode or penalty_mode:
                        assert receive(client)==notice
                        time.sleep(6.3 if repeat_mode else 0.4)  # Any uncancelled update would precede this heartbeat.
                    if jump_mode:client.sendall(frame(encrypt.encrypt(heartbeat)))
                    client.shutdown(socket.SHUT_WR)
                    assert receive(client)==notice and client.recv(1)==b''
                report=future.result(timeout=15)
            output,error=proc.communicate(timeout=12)
            assert proc.returncode==0,(output,error)
            report.update({'proxy_result':output.strip(),'query_response_isolated':True,'no_debugger_or_state_import':True,'local_only':True,'login_forwarding':True,'automatic_self_movement':not overflow,'session_roundtrip':True,'overflow_forwarding':overflow})
            report['local_relay_source_port_preserved']=relay_mode
            report['jump_request_and_post_jump_cipher_sync']=jump_mode
            report['ground_height_during_flight']=ground_height_mode
            report['no_native_jump_required']=flat_mode
            report['native_movement_cancellation']=cancel_mode
            report['own_penalty_cancellation']=penalty_mode
            report['repeat_two_cycles_and_user_stop']=repeat_mode and not (cancel_mode or penalty_mode)
            report['repeat_auto_stop']=repeat_mode and (cancel_mode or penalty_mode)
            report['http_connect_upstream']=upstream_mode
            path=ROOT/('artifacts/field-analysis/unified-overflow-integration.json' if overflow else 'artifacts/field-analysis/jump-repeat-cancel-integration.json' if repeat_mode and cancel_mode else 'artifacts/field-analysis/jump-repeat-penalty-integration.json' if repeat_mode and penalty_mode else 'artifacts/field-analysis/jump-cancel-integration.json' if cancel_mode else 'artifacts/field-analysis/jump-penalty-integration.json' if penalty_mode else 'artifacts/field-analysis/jump-repeat-integration.json' if repeat_mode else 'artifacts/field-analysis/jump-proxy-integration.json' if jump_mode else 'artifacts/field-analysis/independent-proxy-integration.json');path=path.with_name('local-relay-'+path.name) if relay_mode else path;path=path.with_name('flat-'+path.name) if flat_mode else path;path=path.with_name('ground-height-'+path.name) if ground_height_mode else path;path.write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report,indent=2))
        finally:
            proxy_stop.set()
            if proxy_process is not None:proxy_process.terminate();proxy_process.wait(timeout=5)
            if proc.poll() is None:
                try:output,error=proc.communicate(timeout=10)
                except subprocess.TimeoutExpired:proc.kill();output,error=proc.communicate()
                print('probe:',output,error)
if __name__=='__main__':
    if '--connect-proxy-child' in sys.argv:connect_proxy_child()
    else:main()
