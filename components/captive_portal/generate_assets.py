"""Generate single-copy gzip assets for the shared captive portal."""
import gzip, hashlib, sys
from pathlib import Path
root = Path(__file__).resolve().parent
lines = ['// Generated; do not edit.', '#include "compressed_assets.h"']
for name, filename in [('root', 'root.html'), ('styles', 'styles.css'), ('alpine', 'alpine.min.js')]:
    raw = (root / filename).read_bytes()
    data = gzip.compress(raw, compresslevel=9, mtime=0)
    assert data[:3] == b'\x1f\x8b\x08' and data[3] == 0
    lines.append('static const unsigned char portal_' + name + '_gzip[] = {' + ','.join(map(str, data)) + '};')
    etag = hashlib.sha256(raw).hexdigest()[:24]
    gzip_etag = hashlib.sha256(data).hexdigest()[:24]
    lines.append(f'static const compressed_asset_t portal_{name} = {{portal_{name}_gzip, sizeof(portal_{name}_gzip), {len(raw)}, "\\\"{etag}\\\"", "\\\"{gzip_etag}\\\""}};')
Path(sys.argv[1]).write_text('\n'.join(lines) + '\n')
