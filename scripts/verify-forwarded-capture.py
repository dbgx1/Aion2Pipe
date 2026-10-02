"""Bounded regression: capture an already-established local TCP proxy leg."""
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
with socket.socket() as server:
    server.bind(('127.0.0.1', 0))
    server.listen(1)
    server.settimeout(10)
    port = server.getsockname()[1]
    command = f"""
$c = [Net.Sockets.TcpClient]::new('127.0.0.1', {port})
$s = $c.GetStream(); $b = New-Object byte[] 128
for ($i=0; $i -lt 5; $i++) {{ $n = $s.Read($b,0,$b.Length); if ($n -le 0) {{ break }}; $s.Write($b,0,$n) }}
Start-Sleep -Seconds 5
$c.Dispose()
"""
    client = subprocess.Popen(['powershell.exe', '-NoProfile', '-Command', command],
                              creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        with server.accept()[0] as connection:
            connection.settimeout(10)
            # Establish both TCP owners before starting WinDivert FLOW tracking.
            probe = subprocess.Popen([str(ROOT/'build/Release/capture_probe.exe'),
                                      'powershell.exe', '4'], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(1)
            for i in range(5):
                payload = f'local-forwarding-regression-{i}'.encode()
                connection.sendall(payload)
                received = bytearray()
                while len(received) < len(payload):
                    received.extend(connection.recv(len(payload)-len(received)))
                assert received == payload
                time.sleep(.08)
            output, error = probe.communicate(timeout=12)
            assert probe.returncode == 0, error
            target_lines = [line for line in output.splitlines() if line.startswith(str(client.pid)+' ')]
            assert sum(' RX ' in line for line in target_lines) >= 5, output
            assert sum(' TX ' in line for line in target_lines) >= 5, output
            report = 'Existing local TCP connection: target RX >= 5, TX >= 5; PASS\n'+output
            (ROOT/'artifacts/forwarded-capture-check.txt').write_text(report, encoding='utf-8')
            print(report)
    finally:
        try:
            client.wait(timeout=5)
        except subprocess.TimeoutExpired:
            client.kill()
