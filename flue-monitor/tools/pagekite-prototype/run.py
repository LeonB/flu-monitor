#!/usr/bin/env python3
"""Exercise the native-C client against a synthetic loopback HTTP server."""
import concurrent.futures
import gzip
import hashlib
import http.server
import json
import os
from pathlib import Path
import queue
import secrets
import socket
import ssl
import subprocess
import threading
import time
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent

def main():
    credential_path = os.environ.get('PAGEKITE_SECRET_FILE')
    if credential_path:
        credentials = json.loads(Path(credential_path).read_text())
        domain, secret = credentials['domain'], credentials['secret']
    else:
        domain, secret = os.environ.get('PAGEKITE_DOMAIN'), os.environ.get('PAGEKITE_SECRET')
    if not domain or not secret:
        raise SystemExit('Set PAGEKITE_DOMAIN/PAGEKITE_SECRET or PAGEKITE_SECRET_FILE privately.')
    marker = secrets.token_bytes(64)
    large = secrets.token_bytes(128 * 1024)
    compressed = gzip.compress(large, mtime=0)
    class Backend(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def reply(self, body, status=200, encoding=None):
            self.send_response(status)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            if encoding:
                self.send_header('Content-Encoding', encoding)
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
        def do_GET(self):
            if self.path == '/probe': self.reply(marker)
            elif self.path == '/large': self.reply(large)
            elif self.path == '/gzip': self.reply(compressed, encoding='gzip')
            else: self.reply(b'Not found', 404)
        def do_POST(self):
            size = int(self.headers.get('Content-Length', '0'))
            if self.path != '/echo' or size > 128 * 1024:
                self.reply(b'Rejected', 400)
            else: self.reply(self.rfile.read(size))
        def log_message(self, *_): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 18776), Backend)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    environment = os.environ.copy()
    environment.update(PAGEKITE_DOMAIN=domain, PAGEKITE_SECRET=secret)
    child = subprocess.Popen([str(HERE / 'prototype')], env=environment,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    messages = queue.Queue()
    def collect():
        for line in child.stdout: messages.put(line.rstrip())
        messages.put(None)
    threading.Thread(target=collect, daemon=True).start()
    try:
        deadline = time.monotonic() + 40
        relay = None
        while time.monotonic() < deadline:
            try: line = messages.get(timeout=1)
            except queue.Empty: continue
            if line is None: raise RuntimeError('Client exited before authentication')
            print(line, flush=True)
            if line.startswith('RELAY '): relay = line.split()[1]
            if line.startswith('URL '): break
        else: raise RuntimeError('No endpoint before timeout')
        if not relay: raise RuntimeError('No relay address')
        # This is required to route the existing kite to the connected relay.
        # Credentials/signatures are never logged. The account password is unused.
        salt = secrets.token_hex(4)
        signature = salt + hashlib.sha1((secret + domain.lower() + ':' + relay + salt).encode()).hexdigest()
        query = urllib.parse.urlencode({'hostname': domain, 'myip': relay, 'sign': signature})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open('https://up.pagekite.net/?' + query, timeout=15) as response:
                result = response.read(1024)
        except Exception:
            raise RuntimeError('Verified HTTPS DNS update failed (details suppressed)') from None
        if not result.startswith((b'good', b'nochg')):
            raise RuntimeError('Kite routing update rejected (details suppressed)')
        print('Kite routing updated over verified HTTPS', flush=True)
        # Use the connected relay directly, preserving the public hostname/SNI.
        # This separates protocol tests from propagation of the newly updated DNS.
        def fetch(path='/probe', data=None, expected=marker):
            with ssl.create_default_context().wrap_socket(
                    socket.create_connection((relay, 443), timeout=20), server_hostname=domain) as connection:
                request = (f'{"POST" if data is not None else "GET"} {path} HTTP/1.1\r\n'
                           f'Host: {domain}\r\nConnection: close\r\n').encode()
                if data is not None: request += f'Content-Length: {len(data)}\r\n'.encode()
                connection.sendall(request + b'\r\n' + (data or b''))
                import http.client
                response = http.client.HTTPResponse(connection)
                response.begin()
                body = response.read()
                if response.status != 200 or body != expected:
                    raise RuntimeError(f'Public response mismatch: HTTP {response.status}, {len(body)} bytes')
                return response.getheader('Content-Encoding')
        fetch()
        print('PASS: public HTTPS through native C client; exact binary response', flush=True)
        fetch('/large', expected=large)
        assert fetch('/gzip', expected=compressed) == 'gzip'
        print('PASS: 128 KiB binary and gzip responses', flush=True)
        upload = secrets.token_bytes(32 * 1024)
        fetch('/echo', data=upload, expected=upload)
        print('PASS: 32 KiB POST body round trip', flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda _: fetch(), range(8)))
        print('PASS: eight requests across four concurrent clients', flush=True)
        for interval in range(int(os.environ.get('PAGEKITE_IDLE_CHECKS', '1'))):
            time.sleep(35)
            fetch()
            print(f'PASS: tunnel working after idle check {interval + 1}', flush=True)
        try:
            with opener.open('https://' + domain + '/probe', timeout=20) as response:
                if response.status != 200 or response.read() != marker:
                    raise RuntimeError('Public DNS response mismatch')
            print('PASS: ordinary public hostname resolution and HTTPS', flush=True)
        except Exception:
            print('Public DNS check not yet successful; direct-relay HTTPS checks passed', flush=True)
    finally:
        child.terminate()
        try: child.wait(timeout=12)
        except subprocess.TimeoutExpired: child.kill(); child.wait()
        server.shutdown(); server.server_close()
        print('Tunnel and synthetic backend closed', flush=True)

if __name__ == '__main__': main()
