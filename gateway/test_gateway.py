#!/usr/bin/env python3
"""
Integration test for the SRR WebAudio HTTP & WebSocket Gateway.
Verifies HTTP serving and RFC 6455 WebSocket bidirectional bridging to synth_server.
"""

import socket
import struct
import subprocess
import time
import os
import sys
import json
import urllib.request
import base64
import hashlib

def test_http_endpoint():
    print("[TEST 1] Verifying HTTP Dashboard Serving...")
    url = "http://127.0.0.1:8080/index.html"
    req = urllib.request.urlopen(url, timeout=2)
    assert req.status == 200
    content = req.read().decode('utf-8')
    assert "SRR Synthesizer WebAudio Dashboard" in content
    print("  -> HTTP Dashboard served successfully (200 OK).\n")

def test_websocket_gateway():
    print("[TEST 2] Verifying RFC 6455 WebSocket Bridge to SRR...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8081))

    # 1. Send WebSocket Upgrade Handshake
    ws_key = base64.b64encode(os.urandom(16)).decode('utf-8')
    handshake = (
        f"GET / HTTP/1.1\r\n"
        f"Host: 127.0.0.1:8081\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {ws_key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(handshake.encode('utf-8'))

    # Read Handshake Response
    resp = s.recv(1024).decode('utf-8')
    assert "101 Switching Protocols" in resp
    print("  -> WebSocket Upgrade handshake successful (101 Switching Protocols).")

    def ws_send(data_dict):
        msg = json.dumps(data_dict).encode('utf-8')
        mask_key = os.urandom(4)
        masked_payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(msg))
        frame = bytearray([0x81, 0x80 | len(msg)])
        frame.extend(mask_key)
        frame.extend(masked_payload)
        s.sendall(frame)

    def ws_recv():
        b1_b2 = s.recv(2)
        b1, b2 = b1_b2[0], b1_b2[1]
        payload_len = b2 & 0x7F
        payload = s.recv(payload_len)
        return json.loads(payload.decode('utf-8'))

    # 2. Test Synchronous Request via WebSocket: "STATUS"
    ws_send({"action": "send", "cmd": "STATUS"})
    res = ws_recv()
    assert res.get("status") == "OK"
    reply = res.get("reply", "")
    print(f"  -> WebSocket received SRR reply: '{reply}'")
    assert "STATUS active_voices" in reply

    # 3. Test Parameter Change via WebSocket: "SET cutoff 2450.0"
    ws_send({"action": "send", "cmd": "SET cutoff 2450.0"})
    res = ws_recv()
    assert res.get("status") == "OK"
    reply = res.get("reply", "")
    print(f"  -> WebSocket received SRR reply: '{reply}'")
    assert "OK cutoff=2450.0" in reply

    # 4. Test Pulse via WebSocket: clock tick
    ws_send({"action": "pulse", "code": 1, "val": 8})
    res = ws_recv()
    assert res.get("status") == "OK" and res.get("pulse_sent") is True
    print("  -> WebSocket sent SRR Pulse: OK")

    s.close()
    print("  -> WebSocket Gateway test PASSED.\n")

if __name__ == "__main__":
    base_dir = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.dirname(base_dir)
    server_bin = os.path.join(root_dir, "examples", "synth_engine", "synth_server")
    gateway_script = os.path.join(base_dir, "web_gateway.py")

    # Start synth_server
    server_proc = subprocess.Popen([server_bin], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.1)

    # Start web_gateway
    gw_proc = subprocess.Popen([sys.executable, gateway_script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

    try:
        test_http_endpoint()
        test_websocket_gateway()
        print("ALL BROWSER GATEWAY TESTS PASSED SUCCESSFULLY!")
    finally:
        gw_proc.kill()
        gw_proc.wait()
        server_proc.kill()
        server_proc.wait()
