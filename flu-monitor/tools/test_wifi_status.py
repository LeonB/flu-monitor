"""Host check of real display RSSI freshness and session-close code."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'main/ws_server.c').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = '''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
typedef void *httpd_handle_t;
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)
static int s_clients_mutex=1, s_display_wifi_fd=-1, s_display_wifi_rssi;
static int s_client_count, s_client_fds[8];
static uint32_t s_client_generations[8];
static int64_t s_display_wifi_us, now_us;
static void xSemaphoreTake(int mutex, int timeout) {}
static void xSemaphoreGive(int mutex) {}
static int64_t esp_timer_get_time(void) { return now_us; }
static int close(int fd) { return 0; }
'''
code += function('void ws_server_session_close(') + '\n'
code += function('bool ws_server_display_wifi(') + '\n'
code += '''
int main(void) {
    int rssi=99;
    s_clients_mutex=0;
    assert(!ws_server_display_wifi(&rssi));
    s_clients_mutex=1;
    assert(!ws_server_display_wifi(&rssi));
    s_display_wifi_fd=4; s_display_wifi_rssi=-62; s_display_wifi_us=1000000;
    now_us=1000001;
    assert(ws_server_display_wifi(&rssi) && rssi==-62);
    now_us=36000000;
    assert(!ws_server_display_wifi(&rssi));
    now_us=2000000;
    ws_server_session_close(0,5);
    assert(ws_server_display_wifi(&rssi));
    ws_server_session_close(0,4);
    assert(!ws_server_display_wifi(&rssi));
    // Reusing the same descriptor without a new report must remain offline.
    assert(!ws_server_display_wifi(&rssi));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'wifi.c'
    binary = Path(directory) / 'wifi'
    path.write_text(code)
    subprocess.run(['cc', '-std=c11', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: actual RSSI freshness, expiry, disconnect and reused-descriptor handling')
