"""Local-only WinDivert reconnect and closed-route retention integration."""
import argparse, concurrent.futures, pathlib, socket, subprocess, time
parser=argparse.ArgumentParser()
parser.add_argument('--count',type=int,default=40)
parser.add_argument('--interval',type=float,default=0)
args=parser.parse_args()
assert 1<=args.count<=10000 and 0<=args.interval<=1
ROOT=pathlib.Path(__file__).resolve().parents[1]
with socket.socket() as listener:
    listener.bind(('127.0.0.1',0));listener.listen(8);listener.settimeout(20)
    port=listener.getsockname()[1]
    process=subprocess.Popen([str(ROOT/'build/Release/query_proxy_probe.exe'),str(port),'--lifecycle',str(args.count)],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        assert process.stdout.readline().strip()=='READY'
        def server():
            for _ in range(args.count):
                with listener.accept()[0] as peer:
                    peer.settimeout(10);data=peer.recv(1024);assert data==bytes([7,0x34,0x12,0xaa]);peer.sendall(data)
                    assert peer.recv(1)==b''
        with concurrent.futures.ThreadPoolExecutor(1) as executor:
            task=executor.submit(server)
            for i in range(args.count):
                with socket.create_connection(('127.0.0.1',port),timeout=10) as client:
                    client.sendall(bytes([7,0x34,0x12,0xaa]));client.shutdown(socket.SHUT_WR)
                    data=b''
                    while True:
                        part=client.recv(1024)
                        if not part:break
                        data+=part
                    assert data==bytes([7,0x34,0x12,0xaa]),i
                if (i+1)%50==0: print(f'Forwarded {i+1}/{args.count}',flush=True)
                if args.interval: time.sleep(args.interval)
            task.result(timeout=15)
        output,error=process.communicate(timeout=160)
        assert process.returncode==0,(output,error)
        print(output.strip())
    finally:
        if process.poll() is None:process.kill();process.communicate()
