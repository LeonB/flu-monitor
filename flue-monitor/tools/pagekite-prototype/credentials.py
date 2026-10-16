#!/usr/bin/env python3
"""Obtain a tunnel secret over HTTPS; never save the account password."""
import argparse
import getpass
import json
import os
import xmlrpc.client

parser = argparse.ArgumentParser()
parser.add_argument('--domain', required=True)
parser.add_argument('--output', default='/tmp/flu-pagekite-secret.json')
args = parser.parse_args()
email = input('PageKite account email: ')
password = getpass.getpass('PageKite account password: ')
try:
    secret = xmlrpc.client.ServerProxy('https://pagekite.net/xmlrpc/').getSharedSecret(email, password)
    if not isinstance(secret, str) or len(secret) < 8:
        raise ValueError('Unexpected credential response')
except Exception:
    raise SystemExit('Account authentication failed (details suppressed).') from None
fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as file:
    json.dump({'domain': args.domain, 'secret': secret}, file)
print('Tunnel credential saved with owner-only permissions. Remove it after testing.')
