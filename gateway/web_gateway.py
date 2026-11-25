#!/usr/bin/env python3
"""
Zero-dependency HTTP and WebSocket Gateway for SRR.
Serves static UI and bridges browser WebSocket connections to local SRR Unix Domain Sockets.
"""

import socket
import struct
import threading
import hashlib
import base64
import json
import os
import sys
from http.server import HTTPServer, SimpleHTTPRequestHandler

SRR_MAGIC = 0x53525231
SRR_VERSION = 1
SRR_MSG_REQUEST = 2
SRR_MSG_REPLY = 3
SRR_MSG_ERROR = 4
SRR_MSG_PULSE = 5
SRR_MSG_GOODBYE = 7

HTTP_PORT = 8080
WS_PORT = 8081
SYNTH_SOCK = "/tmp/srr_srr_synth.sock"

# ---------------------------------------------------------------------------
# SRR Socket Bridge Helper
# ---------------------------------------------------------------------------
class SRRBridge:
    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.lock = threading.RLock()
        self.trans_id = 0

    def connect(self):
        with self.lock:
            if self.sock:
                try:
                    self.sock.close()
                except Exception:
                    pass
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(self.sock_path)

    def send_cmd(self, cmd_str):
        with self.lock:
            if not self.sock:
                self.connect()
            self.trans_id += 1
            payload = cmd_str.encode('utf-8') + b'\0'
            hdr = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_REQUEST, 0, self.trans_id, len(payload))
            self.sock.sendall(hdr + payload)

            rep_hdr = self._read_exact(24)
            magic, ver, msg_type, flags, rep_tid, payload_len = struct.unpack("<IHHIQI", rep_hdr)
            if magic != SRR_MAGIC:
                raise ValueError("Invalid SRR magic")

            rep_payload = self._read_exact(payload_len)
            if msg_type == SRR_MSG_REPLY:
                return {"status": "OK", "reply": rep_payload.decode('utf-8', errors='ignore').rstrip('\0')}
            elif msg_type == SRR_MSG_ERROR:
                err_code = struct.unpack("<I", rep_payload[:4])[0] if len(rep_payload) >= 4 else -1
                return {"status": "ERROR", "error_code": err_code}
            else:
                return {"status": "ERROR", "msg_type": msg_type}

    def send_pulse(self, code, val):
        with self.lock:
            if not self.sock:
                self.connect()
            payload = struct.pack("<IIQ", code, 0, val)
            hdr = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_PULSE, 0, 0, len(payload))
            self.sock.sendall(hdr + payload)
            return {"status": "OK", "pulse_sent": True}

    def _read_exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionResetError("SRR socket closed")
            data += chunk
        return data

    def close(self):
        with self.lock:
            if self.sock:
                try:
                    bye = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_GOODBYE, 0, 0, 0)
                    self.sock.sendall(bye)
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None

# ---------------------------------------------------------------------------
# Zero-Dependency RFC 6455 WebSocket Server
# ---------------------------------------------------------------------------
def handle_ws_client(conn, bridge):
    try:
        # 1. Read HTTP Upgrade Request
        header_data = b''
        while b'\r\n\r\n' not in header_data:
            chunk = conn.recv(1024)
            if not chunk:
                return
            header_data += chunk

        lines = header_data.decode('utf-8', errors='ignore').split('\r\n')
        ws_key = None
        for line in lines:
            if line.lower().startswith('sec-websocket-key:'):
                ws_key = line.split(':', 1)[1].strip()
                break

        if not ws_key:
            conn.close()
            return

        # 2. Complete Handshake
        accept_raw = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        accept_sha1 = hashlib.sha1(accept_raw.encode('utf-8')).digest()
        accept_b64 = base64.b64encode(accept_sha1).decode('utf-8')

        handshake = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept_b64}\r\n\r\n"
        )
        conn.sendall(handshake.encode('utf-8'))

        # 3. WebSocket Message Loop
        while True:
            b1_b2 = conn.recv(2)
            if not b1_b2 or len(b1_b2) < 2:
                break
            b1, b2 = b1_b2[0], b1_b2[1]
            opcode = b1 & 0x0F
            if opcode == 8: # Close frame
                break

            is_masked = (b2 & 0x80) != 0
            payload_len = b2 & 0x7F

            if payload_len == 126:
                ext = conn.recv(2)
                payload_len = struct.unpack(">H", ext)[0]
            elif payload_len == 127:
                ext = conn.recv(8)
                payload_len = struct.unpack(">Q", ext)[0]

            mask_key = conn.recv(4) if is_masked else None
            payload = b''
            while len(payload) < payload_len:
                chunk = conn.recv(payload_len - len(payload))
                if not chunk:
                    break
                payload += chunk

            if is_masked:
                payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

            msg_text = payload.decode('utf-8', errors='ignore')
            if not msg_text:
                continue

            try:
                msg = json.loads(msg_text)
                action = msg.get("action", "send")
                if action == "send":
                    res = bridge.send_cmd(msg.get("cmd", ""))
                elif action == "pulse":
                    res = bridge.send_pulse(msg.get("code", 1), msg.get("val", 1))
                else:
                    res = {"status": "ERROR", "error": "unknown_action"}
            except Exception as e:
                res = {"status": "ERROR", "error": str(e)}

            # Send JSON response WebSocket text frame (unmasked)
            resp_bytes = json.dumps(res).encode('utf-8')
            frame = bytearray([0x81]) # Fin + Text frame
            resp_len = len(resp_bytes)
            if resp_len < 126:
                frame.append(resp_len)
            elif resp_len <= 65535:
                frame.append(126)
                frame.extend(struct.pack(">H", resp_len))
            else:
                frame.append(127)
                frame.extend(struct.pack(">Q", resp_len))
            frame.extend(resp_bytes)
            conn.sendall(frame)

    except Exception:
        pass
    finally:
        conn.close()

def run_ws_server(port, sock_path):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', port))
    server.listen(16)
    bridge = SRRBridge(sock_path)
    print(f"[WebSocket] Gateway listening on ws://127.0.0.1:{port} (bridging to {sock_path})")

    while True:
        client, _ = server.accept()
        t = threading.Thread(target=handle_ws_client, args=(client, bridge), daemon=True)
        t.start()

def run_http_server(port, public_dir):
    os.chdir(public_dir)
    server = HTTPServer(('127.0.0.1', port), SimpleHTTPRequestHandler)
    print(f"[HTTP Server] Serving WebAudio Dashboard on http://127.0.0.1:{port}/")
    server.serve_forever()

if __name__ == "__main__":
    base_dir = os.path.dirname(os.path.abspath(__file__))
    public_dir = os.path.join(base_dir, "public")
    sock_path = sys.argv[1] if len(sys.argv) > 1 else SYNTH_SOCK

    ws_thread = threading.Thread(target=run_ws_server, args=(WS_PORT, sock_path), daemon=True)
    ws_thread.start()

    run_http_server(HTTP_PORT, public_dir)
