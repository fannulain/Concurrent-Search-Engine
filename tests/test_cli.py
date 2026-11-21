"""Unit and integration tests for the Python CLI client (`./search`)."""

import base64
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.machinery import SourceFileLoader
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI_PATH = REPO_ROOT / "search"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

search_cli = SourceFileLoader("search_cli", str(CLI_PATH)).load_module()


class MockRestHandler(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args) -> None:
        return

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(length).decode("utf-8"))

        if self.path != "/api/search":
            self.send_response(404)
            self.end_headers()
            return

        if payload.get("query") == "trigger_error":
            body = json.dumps({"error": "Invalid query"}).encode("utf-8")
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        body = json.dumps(
            {
                "results": [
                    {"doc_id": "1001_0", "score": 1.0, "snippet": "Dante inferno passage"},
                ],
                "time_ms": 12,
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def _recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError("Unexpected EOF")
        buf.extend(chunk)
    return bytes(buf)


def _read_ws_frame(conn: socket.socket) -> str:
    first_two = _recv_exact(conn, 2)
    masked = (first_two[1] & 0x80) != 0
    length = first_two[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", _recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", _recv_exact(conn, 8))[0]
    mask_key = _recv_exact(conn, 4) if masked else b""
    payload = _recv_exact(conn, length) if length > 0 else b""
    if masked:
        payload = bytes(payload[i] ^ mask_key[i % 4] for i in range(len(payload)))
    return payload.decode("utf-8")


def _send_ws_frame(conn: socket.socket, text: str) -> None:
    data = text.encode("utf-8")
    length = len(data)
    header = bytearray([0x81])
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(127)
        header.extend(struct.pack("!Q", length))
    conn.sendall(bytes(header) + data)


class MockWebSocketServer:
    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                break
            threading.Thread(target=self._handle_client, args=(conn,), daemon=True).start()

    def _handle_client(self, conn: socket.socket) -> None:
        try:
            raw = bytearray()
            while b"\r\n\r\n" not in raw:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                raw.extend(chunk)
            header_text = raw[: raw.index(b"\r\n\r\n")].decode("ascii", errors="replace")
            ws_key = ""
            for line in header_text.splitlines():
                if line.lower().startswith("sec-websocket-key:"):
                    ws_key = line.split(":", 1)[1].strip()
                    break

            accept = base64.b64encode(hashlib.sha1((ws_key + WS_GUID).encode("ascii")).digest()).decode("ascii")
            handshake = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            )
            conn.sendall(handshake.encode("ascii"))

            req = json.loads(_read_ws_frame(conn))
            query = req.get("query", "")
            if query == "ws_error":
                _send_ws_frame(conn, json.dumps({"stage": "error", "data": {"message": "Bad query"}}))
                return

            _send_ws_frame(
                conn,
                json.dumps(
                    {
                        "stage": "query_expansion",
                        "data": {"original": query, "expanded_terms": ["hell", "underworld"]},
                    }
                ),
            )
            _send_ws_frame(
                conn,
                json.dumps(
                    {
                        "stage": "keyword_search",
                        "data": {
                            "results": [{"doc_id": "1001_0", "score": 1.0, "snippet": "Dante inferno"}],
                            "time_ms": 18,
                        },
                    }
                ),
            )
            _send_ws_frame(conn, json.dumps({"stage": "done"}))
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def close(self) -> None:
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


class TestCliClient(unittest.TestCase):
    rest_server: ThreadingHTTPServer
    rest_thread: threading.Thread
    ws_server: MockWebSocketServer

    @classmethod
    def setUpClass(cls) -> None:
        cls.rest_server = ThreadingHTTPServer(("127.0.0.1", 0), MockRestHandler)
        cls.rest_port = cls.rest_server.server_address[1]
        cls.rest_thread = threading.Thread(target=cls.rest_server.serve_forever, daemon=True)
        cls.rest_thread.start()
        cls.ws_server = MockWebSocketServer()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.rest_server.shutdown()
        cls.rest_server.server_close()
        cls.ws_server.close()

    def test_keyword_mode_success(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(CLI_PATH), "inferno", "--host", f"http://127.0.0.1:{self.rest_port}", "--limit", "5"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        self.assertIn("[1001_0] (score: 1.000)", proc.stdout)
        self.assertIn("Completed in 12 ms", proc.stdout)

    def test_keyword_mode_http_error_exits_nonzero(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(CLI_PATH), "trigger_error", "--host", f"http://127.0.0.1:{self.rest_port}"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Server error (HTTP 400)", proc.stderr)

    def test_stream_mode_success(self) -> None:
        proc = subprocess.run(
            [
                sys.executable,
                str(CLI_PATH),
                "inferno",
                "--mode",
                "stream",
                "--host",
                f"http://127.0.0.1:{self.ws_server.port}",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        self.assertIn("[query_expansion] expanded_terms=['hell', 'underworld']", proc.stdout)
        self.assertIn("[keyword_search] 1 result(s) in 18 ms", proc.stdout)
        self.assertIn("[done]", proc.stdout)

    def test_stdlib_websocket_client_fallback(self) -> None:
        client = search_cli.StdlibWebSocketClient(f"ws://127.0.0.1:{self.ws_server.port}/ws/search", timeout=5.0)
        try:
            client.send_text(json.dumps({"query": "inferno", "limit": 5}))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                while True:
                    finished, code = search_cli.handle_stream_message(client.recv_text())
                    if finished:
                        self.assertEqual(code, 0)
                        break
            output = out.getvalue()
            self.assertIn("[query_expansion]", output)
            self.assertIn("[1001_0]", output)
            self.assertIn("[done]", output)
        finally:
            client.close()

    def test_stream_mode_error_stage_exits_nonzero(self) -> None:
        proc = subprocess.run(
            [
                sys.executable,
                str(CLI_PATH),
                "ws_error",
                "--mode",
                "stream",
                "--host",
                f"http://127.0.0.1:{self.ws_server.port}",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("[error] Bad query", proc.stderr)


if __name__ == "__main__":
    unittest.main()
