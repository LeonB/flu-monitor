"""Live dashboard checks; malformed writes exercise transport without recording events."""
import concurrent.futures
import json
import sys
import time
import urllib.error
import urllib.request

base = sys.argv[1].rstrip('/')
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
def fetch(path, data=None):
    request = urllib.request.Request(base + path, data=data, headers={'Accept-Encoding':'gzip', 'Content-Type':'application/json'})
    try:
        response = opener.open(request, timeout=20)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, response.headers, response.read()

def get_json(path):
    status, _, body = fetch(path)
    assert status == 200, (path, status)
    return json.loads(body)

for attempt in range(20):
    try:
        probe = get_json('/probe')
        if probe['mode'] == 'pagekite-dashboard': break
    except Exception: pass
    time.sleep(2)
else: raise SystemExit('Dashboard tunnel unavailable')
print('PASS ESP32 dashboard tunnel online:', probe, flush=True)
settings = get_json('/api/settings')
assert settings['credentials_redacted'] is True
assert 'remote_read_only' not in settings
assert 'google_sheets_secret' not in settings
assert 'google_sheets_webhook_url' not in settings
assert 'zone_cold_color' in settings
assert 'pagekite_secret' not in settings
assert 'pagekite_relay' not in settings
assert settings['pagekite_secret_set'] is True
assert settings['pagekite_enabled'] is True
assert get_json('/api/reading')['thermocouple_ok']
assert 'samples' in get_json('/api/history')
assert isinstance(get_json('/api/events'), list)
for attempt in range(20):
    wifi = get_json('/api/devices/wifi')
    if wifi['monitor']['connected'] and wifi['display']['connected']: break
    time.sleep(2)
else: raise AssertionError('Monitor/display WiFi did not reconnect')
print('PASS live readings/history/events/WiFi; remote settings contain no stored credentials', flush=True)
paths = ['/', '/dashboard.js', '/dashboard.css', '/burn-graph.js', '/alpinejs.min.js', '/vendor/uplot.min.js']
for repeat in range(3):
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        replies = list(pool.map(fetch, paths))
    assert all(status == 200 and body for status, _, body in replies), [(p, r[0]) for p,r in zip(paths,replies)]
print('PASS 18 asset requests across six concurrent clients', flush=True)
for path in ['/ota?secret=invalid', '/ws', '/api/wifi', '/api/tunnel/settings']:
    assert fetch(path)[0] == 404, path
assert 'display_applied' in get_json('/api/settings/status')
assert fetch('/api/event', b'{"event":"not_a_real_event"}')[0] == 400
assert fetch('/api/settings', b'{"minimum_brightness":-1}')[0] == 400
assert fetch('/ota', b'{}')[0] == 404
print('PASS settings/event POSTs reach validation; display acknowledgement available; OTA/configuration routes blocked', flush=True)
print('Final probe:', get_json('/probe'), flush=True)
