"""Check live compression, cache validation and identity fallback after a flash."""
import gzip
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.request import build_opener, ProxyHandler, Request

root = Path(__file__).resolve().parents[1]
opener = build_opener(ProxyHandler({}))
host = sys.argv[1]

def get(path, headers):
    try:
        response = opener.open(Request(f"http://{host}{path}", headers=headers), timeout=20)
    except HTTPError as error:
        response = error
    with response:
        return response.status, response.headers, response.read()

assets = {
    "/": root / "main/web_ui/dashboard.html",
    "/dashboard.js": root / "main/web_ui/dashboard.js",
    "/dashboard.css": root / "main/web_ui/dashboard.css",
    "/alpinejs.min.js": root.parent / "components/captive_portal/alpine.min.js",
    **{f"/fonts/{p.name}": p for p in (root / "main/web_ui/fonts").glob("*.woff2")},
}
for path, source in assets.items():
    code, headers, payload = get(path, {"Accept-Encoding": "gzip"})
    assert code == 200, (path, code)
    compressed = source.suffix != ".woff2"
    assert (headers.get("Content-Encoding") == "gzip") == compressed, path
    assert (gzip.decompress(payload) if compressed else payload) == source.read_bytes(), path
    assert headers["Cache-Control"] == "no-cache", path
    tag = headers["ETag"]
    code, cached, body = get(path, {"Accept-Encoding": "gzip", "If-None-Match": tag})
    assert code == 304 and not body and cached["ETag"] == tag, path
    code, _, body = get(path, {"Accept-Encoding": "gzip", "If-None-Match": '"old-firmware"'})
    assert code == 200 and body == payload, path
    if compressed:
        for encoding in ("identity", "br, gzip;q=0"):
            code, identity, body = get(path, {"Accept-Encoding": encoding, "If-None-Match": tag})
            assert code == 200 and identity.get("Content-Encoding") is None, path
            assert body == source.read_bytes() and identity["ETag"] != tag, path
        assert headers["Vary"] == "Accept-Encoding", path
    print(f"PASS {path}: {len(source.read_bytes())} -> {len(payload)} bytes; 304 and stale-tag recovery", flush=True)
