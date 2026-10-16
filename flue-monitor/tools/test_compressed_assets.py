"""Check generated assets; optionally test streaming C fallback using upstream miniz.

python3 tools/test_compressed_assets.py /path/to/miniz-v114.c
The optional reference source implements the same tinfl API as ESP32 ROM.
"""
import gzip
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
portal = root.parent / 'components/captive_portal'
assets = list((root / 'main/web_ui').glob('*.js')) + list((root / 'main/web_ui').glob('*.html')) + list((root / 'main/web_ui').glob('*.css')) + list((root / 'main/web_ui/vendor').glob('*.js')) + list((root / 'main/web_ui/vendor').glob('*.css')) + [portal / n for n in ('root.html', 'styles.css', 'alpine.min.js')]
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    subprocess.run([sys.executable, str(root / 'tools/generate_web_assets.py'), str(tmp / 'monitor.h')], check=True)
    subprocess.run([sys.executable, str(portal / 'generate_assets.py'), str(tmp / 'portal_assets_generated.h')], check=True)
    for header in ('monitor.h', 'portal_assets_generated.h'):
        generated = (tmp / header).read_text()
        for array in re.findall(r'unsigned char \w+\[\] = \{([^}]+)\}', generated):
            encoded = bytes(int(n) for n in array.split(',') if n.strip())
            decoded = gzip.decompress(encoded)
            assert encoded[:4] == b'\x1f\x8b\x08\x00'
            assert any(decoded == source.read_bytes() for source in assets)
            assert hashlib.sha256(encoded).hexdigest()[:24] in generated
    generated = (tmp / 'monitor.h').read_text()
    assert 'asset_css_gzip[]' not in generated and 'asset_alpine_js_gzip[]' not in generated
    print('PASS: generated gzip payloads, tags, shared CSS and Alpine deduplication')
    if len(sys.argv) < 2:
        sys.exit(0)
    reference = Path(sys.argv[1]).resolve()
    (tmp / 'miniz.h').write_text('#define MINIZ_HEADER_FILE_ONLY\n#include "' + str(reference) + '"\n')
    (tmp / 'esp_http_server.h').write_text('''#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
typedef struct { const char *encoding; int fail; } httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN -1
int httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_resp_send_err(httpd_req_t *, int, const char *);
int httpd_resp_set_status(httpd_req_t *, const char *);
int httpd_resp_send(httpd_req_t *, const char *, int);
int httpd_resp_send_chunk(httpd_req_t *, const char *, size_t);
''')
    (tmp / 'harness.c').write_text('''#include "compressed_assets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *n,char *v,size_t z) {
 if (!r->encoding || strlen(r->encoding)>=z) return -1; strcpy(v,r->encoding); return 0;
}
int httpd_resp_send_err(httpd_req_t *r,int c,const char *m) { return -1; }
int httpd_resp_set_status(httpd_req_t *r,const char *s) { return 0; }
int httpd_resp_send(httpd_req_t *r,const char *s,int z) { return -1; }
int httpd_resp_send_chunk(httpd_req_t *r,const char *s,size_t z) {
 if(r->fail) return -1;
 return z && fwrite(s,1,z,stdout)!=z ? -1 : 0;
}
int main(int argc,char **argv) {
 httpd_req_t r={0};
 const char *encodings[]={"gzip","br, gzip;q=0","identity","br, gzip;q=0.5",NULL};
 const bool accepted[]={true,false,false,true,false};
 for(int i=0;i<5;i++){r.encoding=encodings[i];assert(compressed_asset_accepts_gzip(&r)==accepted[i]);}
 FILE *f=fopen(argv[1],"rb"); assert(f); fseek(f,0,SEEK_END); size_t n=ftell(f); rewind(f);
 unsigned char *b=malloc(n); assert(fread(b,1,n,f)==n); fclose(f);
 r.fail=argc>3;
 int result=compressed_asset_send_identity(&r,b,n,strtoul(argv[2],NULL,10));free(b);
 return result==0 ? 0 : 1;
}
''')
    executable = tmp / 'decode'
    subprocess.run(['cc', '-w', '-DMINIZ_NO_ARCHIVE_APIS', '-DMINIZ_NO_STDIO', '-DMINIZ_NO_ZLIB_APIS', '-I'+str(tmp), '-I'+str(portal / 'include'), str(portal / 'compressed_assets.c'), str(tmp / 'harness.c'), str(reference), '-o', str(executable)], check=True)
    for source in assets:
        raw = source.read_bytes()
        encoded = tmp / 'input.gz'
        encoded.write_bytes(gzip.compress(raw, mtime=0))
        result = subprocess.run([str(executable), str(encoded), str(len(raw))], capture_output=True)
        assert result.returncode == 0 and result.stdout == raw, source
        failure = subprocess.run([str(executable), str(encoded), str(len(raw)), 'disconnect'], capture_output=True)
        assert failure.returncode != 0, source
    encoded.write_bytes(b'bad header')
    assert subprocess.run([str(executable), str(encoded), '1'], capture_output=True).returncode != 0
    print('PASS: actual C fallback, multi-chunk byte equality, encoding negotiation, disconnect and invalid-header handling')
