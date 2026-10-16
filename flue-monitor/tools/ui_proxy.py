#!/usr/bin/env python3
"""Serve local UI assets with read-only live API data. No device writes.
Usage: python3 tools/ui_proxy.py http://192.168.1.137 8773
"""
import http.server
import html
import secrets
import sys
from http.cookies import SimpleCookie
from urllib.error import HTTPError, URLError
from urllib.request import Request, build_opener, ProxyHandler
from urllib.parse import parse_qs, urlparse
from pathlib import Path
from mock_server import Handler, WEB_UI_DIR

upstream = sys.argv[1].rstrip('/')
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8773
opener = build_opener(ProxyHandler({}))
LOGIN_PAGE = Path(__file__).with_name('pagekite_auth_preview.html')
PREVIEW_PASSWORD = 'review'
SESSION_TOKEN = secrets.token_urlsafe(32)

class Proxy(Handler):
    def _has_session(self):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get('Cookie', ''))
        except Exception:
            return False
        return cookie.get('flu_auth_preview') is not None and cookie['flu_auth_preview'].value == SESSION_TOKEN

    def _login(self, error='', notice=''):
        source = LOGIN_PAGE.read_text()
        source = source.replace('{{ERROR}}', html.escape(error))
        source = source.replace('{{NOTICE}}', html.escape(notice))
        body = source.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, location, clear_cookie=False):
        self.send_response(303)
        self.send_header('Location', location)
        if clear_cookie:
            self.send_header('Set-Cookie', 'flu_auth_preview=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict')
        self.send_header('Content-Length', '0')
        self.end_headers()

    def _dashboard(self):
        source = (WEB_UI_DIR / 'dashboard.html').read_text()
        password_group = '''<div class="field-group">
        <h6>Dashboard access</h6>
        <div class="field-list">
          <form id="auth-preview-password-form" method="post" action="/auth/password" style="margin:0" onsubmit="submitAuthPreviewPassword(event,this)">
            <div class="text-field">
              <label for="auth-current-password">Dashboard password</label>
              <small class="field-help" style="margin-bottom:8px">Protect dashboard access on your home network and through PageKite. Changing the password signs out existing sessions.</small>
              <label for="auth-current-password">Current password</label>
              <input id="auth-current-password" name="current_password" class="input" type="password" autocomplete="current-password" required>
            </div>
            <div class="text-field" style="padding-top:0">
              <label for="auth-new-password">New password</label>
              <input id="auth-new-password" name="new_password" class="input" type="password" autocomplete="new-password" minlength="8" maxlength="128" required>
            </div>
            <div class="text-field" style="padding-top:0">
              <label for="auth-confirm-password">Confirm new password</label>
              <input id="auth-confirm-password" name="confirm_password" class="input" type="password" autocomplete="new-password" minlength="8" maxlength="128" required>
            </div>
            <div class="text-field" style="padding-top:0">
              <button class="btn btn-primary" type="submit" style="width:100%;height:46px">Update password</button>
              <div id="auth-preview-password-error" role="alert" style="min-height:18px;margin-top:8px;color:#a94236;font-size:12px"></div>
            </div>
          </form>
          <div class="field-row" style="border-top:1px solid var(--color-divider);margin:0 8px;padding:14px 8px 10px">
            <span class="field-label">Sign out<small class="field-help">End this browser session.</small></span>
            <a href="/auth/logout" class="btn btn-secondary" style="height:40px;display:inline-flex;align-items:center;text-decoration:none">Sign out</a>
          </div>
        </div>
      </div>'''
        settings_title = '<h2 style="margin:0">Tuning</h2>'
        source = source.replace(settings_title, settings_title + '\n\n      ' + password_group, 1)
        script = '''<script>
          async function submitAuthPreviewPassword(event, form) {
            event.preventDefault();
            const message = document.getElementById('auth-preview-password-error');
            const button = form.querySelector('button[type="submit"]');
            message.textContent = '';
            button.disabled = true;
            try {
              const response = await fetch(form.action, {method:'POST', body:new URLSearchParams(new FormData(form))});
              const result = await response.json();
              if (!response.ok) { message.textContent = result.error || 'Password was not changed.'; return; }
              window.location.assign('/auth/login?changed=1');
            } catch (_) {
              message.textContent = 'Could not update the password. Try again.';
            } finally {
              button.disabled = false;
            }
          }
        </script>'''
        source = source.replace('</body>', script + '\n</body>', 1)
        body = source.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == '/auth/logout':
            return self._redirect('/', clear_cookie=True)
        if path == '/auth/login' and not self._has_session():
            query = parse_qs(urlparse(self.path).query)
            notice = 'Password updated. Sign in again.' if query.get('changed') == ['1'] else ''
            return self._login(notice=notice)
        if not self._has_session():
            return self._login()
        if path == '/':
            return self._dashboard()
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
        global PREVIEW_PASSWORD, SESSION_TOKEN
        path = urlparse(self.path).path
        length = min(int(self.headers.get('Content-Length', 0)), 4096)
        form = parse_qs(self.rfile.read(length).decode('utf-8', errors='replace'))
        if path == '/auth/login':
            if form.get('password', [''])[0] == PREVIEW_PASSWORD:
                self.send_response(303)
                self.send_header('Location', '/')
                self.send_header('Set-Cookie', 'flu_auth_preview=' + SESSION_TOKEN + '; Path=/; Max-Age=1800; HttpOnly; SameSite=Strict')
                self.send_header('Content-Length', '0')
                self.end_headers()
            else:
                self._login('That password did not match. Try again.')
            return
        if not self._has_session():
            self._json({'error': 'Please sign in first'}, 401)
            return
        if path == '/auth/password':
            current = form.get('current_password', [''])[0]
            new = form.get('new_password', [''])[0]
            confirm = form.get('confirm_password', [''])[0]
            if current != PREVIEW_PASSWORD:
                return self._json({'error': 'Current password is incorrect. The password was not changed.'}, 400)
            if len(new) < 8 or len(new) > 128:
                return self._json({'error': 'Choose a password between 8 and 128 characters.'}, 400)
            if new != confirm:
                return self._json({'error': 'The new passwords do not match. The password was not changed.'}, 400)
            PREVIEW_PASSWORD = new
            SESSION_TOKEN = secrets.token_urlsafe(32)  # invalidate every existing session
            return self._json({'success': True})
        self._json({'error': 'Local graph preview is read-only'}, 405)

print(f'Local UI: http://localhost:{port}/ — live data from {upstream}; auth preview password: {PREVIEW_PASSWORD}', flush=True)
http.server.ThreadingHTTPServer(('localhost', port), Proxy).serve_forever()
