#!/usr/bin/env python3
"""Serve local UI assets with read-only live API data. No device writes.
Usage: python3 tools/ui_proxy.py http://192.168.1.137 8773
"""
import http.server
import sys
from urllib.error import HTTPError, URLError
from urllib.request import Request, build_opener, ProxyHandler
from mock_server import Handler

upstream = sys.argv[1].rstrip('/')
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8773
opener = build_opener(ProxyHandler({}))

class Proxy(Handler):
    def do_GET(self):
        if not self.path.startswith('/api/'):
            return super().do_GET()
        try:
            with opener.open(Request(upstream + self.path), timeout=10) as response:
                body = response.read()
                self.send_response(response.status)
                self.send_header('Content-Type', response.headers.get('Content-Type', 'application/json'))
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        except (HTTPError, URLError, TimeoutError):
            self._json({'error': 'Monitor unavailable'}, 502)

    def do_POST(self):
        self._json({'error': 'Local graph preview is read-only'}, 405)

print(f'Local UI: http://localhost:{port}/ — live data from {upstream}', flush=True)
http.server.ThreadingHTTPServer(('localhost', port), Proxy).serve_forever()
