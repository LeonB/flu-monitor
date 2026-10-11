"""Reversible live PageKite configuration checks on the identified monitor.

Usage: python3 tools/test_pagekite_configuration.py MONITOR_IP SECRET_FILE
SECRET_FILE is the owner-only JSON produced by pagekite-prototype/credentials.py.
The current settings are restored in finally; secrets are never printed.
"""
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

lan = 'http://' + sys.argv[1]
credential = json.loads(Path(sys.argv[2]).read_text())
public = 'https://' + credential['domain']
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
last_uptime = None

def fetch(base, path, data=None):
    request = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(), headers={'Content-Type': 'application/json'})
    try:
        with opener.open(request, timeout=5) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, None

def get(base, path='/api/settings'):
    status, data = fetch(base, path)
    assert status == 200
    return data

def save(base, settings):
    status, result = fetch(base, '/api/settings', settings)
    assert status == 200 and result['success'] and result['pagekite_secret_set']
    return result

def online(expected, timeout=35):
    global last_uptime
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            status, probe = fetch(public, '/probe')
            present = status == 200 and probe and probe.get('mode') == 'pagekite-dashboard'
        except Exception:
            present = False
        if present:
            if last_uptime is not None:
                assert probe['uptime_s'] >= last_uptime, 'Unexpected monitor restart during configuration test'
            last_uptime = probe['uptime_s']
        if bool(present) == expected:
            return probe
        time.sleep(1)
    raise AssertionError('Tunnel did not reach expected state')

assert get(lan, '/api/reading')['thermocouple_ok']
baseline = get(lan)
configured = dict(baseline, pagekite_enabled=True, pagekite_domain=credential['domain'], pagekite_secret=credential['secret'])
try:
    save(lan, configured)
    online(True)
    local = get(lan)
    remote = get(public)
    assert local['pagekite_secret_set'] and remote['pagekite_secret_set']
    assert 'pagekite_secret' not in local and 'pagekite_secret' not in remote
    assert 'google_sheets_secret' not in remote
    print('PASS configure existing kite without rebuild; local/public responses mask shared secret', flush=True)
    for blank in (False, True):
        update = get(public)
        if blank:
            update['pagekite_secret'] = ''
        save(public, update)
        online(True)
    print('PASS public saves preserve omitted and blank shared secret', flush=True)
    # A remote disable must deliver its successful save response before disconnect.
    update = get(public)
    update['pagekite_enabled'] = False
    save(public, update)
    online(False)
    assert get(lan)['pagekite_secret_set']
    update = get(lan)
    update['pagekite_enabled'] = True
    update['pagekite_secret'] = ''
    save(lan, update)
    online(True)
    print('PASS remote disable acknowledges before closing; LAN re-enable reuses saved secret', flush=True)
    update = get(public)
    update['pagekite_secret'] = 'invalid-test-secret-for-reconnect'
    save(public, update)
    online(False)
    save(lan, configured)
    online(True)
    print('PASS replacement secret is applied at runtime; restoring valid secret reconnects', flush=True)
    actual = get(lan)
    for key, value in baseline.items():
        if key not in ('settings_revision', 'pagekite_enabled', 'pagekite_domain', 'pagekite_secret_set'):
            assert actual[key] == value, key
    print('PASS all existing settings and Google Sheets credentials preserved', flush=True)
finally:
    # Keep the user's existing tunnel enabled; reverse only temporary test edits.
    save(lan, configured)
print('Final probe:', online(True), flush=True)
