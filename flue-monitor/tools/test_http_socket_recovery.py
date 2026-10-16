"""Live smoke test: python3 tools/test_http_socket_recovery.py <monitor-ip>.
Uses readings/status only; never changes saved settings.
"""
import base64
import http.client
import os
import socket
import sys
import time

host = sys.argv[1]
held = []
try:
    for _ in range(7):
        connection = http.client.HTTPConnection(host, timeout=8)
        connection.request('GET', '/api/reading')
        response = connection.getresponse()
        assert response.status == 200
        response.read()
        held.append(connection)
    # An eighth HTTP connection must be serviced even with idle sessions held.
    probe = http.client.HTTPConnection(host, timeout=8)
    probe.request('GET', '/api/settings/status')
    response = probe.getresponse()
    assert response.status == 200
    response.read()
    probe.close()
    print('PASS: new settings/status request works with seven idle HTTP connections', flush=True)
    # Keep remaining connections around so serial diagnostics can classify them.
    time.sleep(32)
finally:
    for connection in held:
        connection.close()

for _ in range(10):
    client = socket.create_connection((host, 80), timeout=8)
    key = base64.b64encode(os.urandom(16)).decode()
    client.sendall((f'GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
                    f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                    'Sec-WebSocket-Version: 13\r\n\r\n').encode())
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = client.recv(4096)
        assert chunk
        data += chunk
    assert data.startswith(b'HTTP/1.1 101'), data[:100]
    # TCP disconnect without a WebSocket close frame.
    client.close()
probe = http.client.HTTPConnection(host, timeout=8)
probe.request('GET', '/api/settings/status')
response = probe.getresponse()
assert response.status == 200
response.read()
probe.close()
print('PASS: ten WS disconnects without close frames; HTTP remains available')
