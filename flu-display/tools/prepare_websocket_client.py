"""Build a project-local WS client copy that rejects redirects and closes them.

The monitor WS endpoint has a fixed URI. A captive-portal redirect is a
failed handshake, not a new endpoint. Never modify managed component files.
"""
from pathlib import Path
import sys


def prepare(source):
    marker = "#if WS_TRANSPORT_REDIRECT_HEADER_SUPPORT\n            else if (WS_HTTP_REDIRECT(result)) {"
    if source.count(marker) != 1:
        raise RuntimeError("WebSocket dependency changed: review redirect handling before building")
    start = source.index(marker)
    end = source.index("#endif", start) + len("#endif")
    replacement = """#if WS_TRANSPORT_REDIRECT_HEADER_SUPPORT
            else if (WS_HTTP_REDIRECT(result)) {
                // flu-display uses a fixed monitor endpoint. Reject captive-portal
                // redirects, close the handshake socket and use normal retry delay.
                esp_websocket_client_error(client, "Rejected WebSocket HTTP redirect %d", result);
                esp_websocket_client_abort_connection(client, WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT);
                break;
            }
#endif"""
    return source[:start] + replacement + source[end:]


if __name__ == "__main__":
    Path(sys.argv[2]).write_text(prepare(Path(sys.argv[1]).read_text()))
