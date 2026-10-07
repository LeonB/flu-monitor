"""Hardware fault-injection server. Use only with FLU_DISPLAY_TEST_WS_URI.

Write redirect, drop or success to /tmp/flu-ws-test-mode to select behaviour.
Success sends a synthetic 65 C reading. Clear the build URI override and
flash production firmware after testing. Never use this as a real monitor.
"""
import socket, threading, time, pathlib, hashlib, base64, json
modefile = pathlib.Path('/tmp/flu-ws-test-mode')
modefile.write_text('redirect')
lock = threading.Lock()
active = 0
attempts = 0

def client(c):
    global active, attempts
    with lock:
        active += 1
        attempts += 1
        n = attempts
        print(json.dumps(dict(event='open', attempt=n, active=active)), flush=True)
    try:
        c.settimeout(1)
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = c.recv(4096)
            if not chunk:
                return
            data += chunk
        mode = modefile.read_text().strip()
        if mode == 'redirect':
            c.sendall(b'HTTP/1.1 303 See Other\r\nLocation: /ws\r\nContent-Length: 0\r\n\r\n')
        elif mode == 'drop':
            return
        else:
            key = next((l.split(b':', 1)[1].strip() for l in data.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key:')))
            accept = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
            c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
        while True:
            try:
                b = c.recv(4096)
                if not b:
                    break
                if mode == 'success':
                    print(json.dumps(dict(event='ws-data', bytes=len(b))), flush=True)
            except socket.timeout:
                if mode == 'success':
                    b = json.dumps(dict(type='reading', reading=dict(thermocouple_ok=True, thermocouple_c=65, thermocouple_rate_c_per_min=0))).encode()
                    c.sendall(bytes([129, 126]) + len(b).to_bytes(2, 'big') + b)
    except Exception as e:
        print(type(e).__name__, flush=True)
    finally:
        c.close()
        with lock:
            active -= 1
            print(json.dumps(dict(event='close', attempt=n, active=active)), flush=True)
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('0.0.0.0', 8768))
server.listen()
print('Fault server listening on 8768', flush=True)
while True:
    c, a = server.accept()
    threading.Thread(target=client, args=(c,), daemon=True).start()
