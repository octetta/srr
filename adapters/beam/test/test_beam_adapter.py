#!/usr/bin/env python3
"""
Test script verifying BEAM integration:
  1. Direct binary wire protocol (testing the framing logic implemented in srr_client.erl).
  2. BEAM Port Driver binary protocol (testing the stdio packet bridge used by OTP port managers).
"""

import socket
import struct
import subprocess
import time
import sys
import os

SRR_MAGIC = 0x53525231
SRR_VERSION = 1

SRR_MSG_HELLO = 1
SRR_MSG_REQUEST = 2
SRR_MSG_REPLY = 3
SRR_MSG_ERROR = 4
SRR_MSG_PULSE = 5
SRR_MSG_GOODBYE = 7

def test_direct_beam_wire_protocol(sock_path):
    print("[TEST 1] Verifying BEAM Binary Wire Protocol (as in srr_client.erl)...")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)

    # 1. Send Request (trans_id=42, payload="STATUS")
    payload = b"STATUS\0"
    trans_id = 42
    # uint32 magic, uint16 version, uint16 type, uint32 flags, uint64 trans_id, uint32 payload_len
    header = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_REQUEST, 0, trans_id, len(payload))
    s.sendall(header + payload)

    # Read reply header (24 bytes)
    rep_header = s.recv(24)
    magic, ver, msg_type, flags, rep_tid, payload_len = struct.unpack("<IHHIQI", rep_header)

    assert magic == SRR_MAGIC, f"Bad magic: {hex(magic)}"
    assert ver == SRR_VERSION, f"Bad version: {ver}"
    assert msg_type == SRR_MSG_REPLY, f"Expected REPLY(3), got {msg_type}"
    assert rep_tid == trans_id, f"TID mismatch: expected {trans_id}, got {rep_tid}"

    rep_payload = s.recv(payload_len)
    reply_str = rep_payload.decode('utf-8', errors='ignore').rstrip('\0')
    print(f"  -> BEAM received synchronous reply: '{reply_str}'")
    assert "STATUS active_voices" in reply_str

    # 2. Send Pulse (code=1, val=4)
    pulse_payload = struct.pack("<IIQ", 1, 0, 4)
    pulse_hdr = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_PULSE, 0, 0, len(pulse_payload))
    s.sendall(pulse_hdr + pulse_payload)
    print("  -> BEAM sent asynchronous pulse (code=1, val=4)")

    # 3. Goodbye
    bye_hdr = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_GOODBYE, 0, 0, 0)
    s.sendall(bye_hdr)
    s.close()
    print("  -> Wire protocol test PASSED.\n")

def test_beam_port_driver(port_bin_path, service_name):
    print("[TEST 2] Verifying BEAM Port Driver stdio protocol...")
    proc = subprocess.Popen([port_bin_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def send_port_cmd(cmd_tag, data=b""):
        pkt = bytes([cmd_tag]) + data
        length = struct.pack("!I", len(pkt))
        proc.stdin.write(length + pkt)
        proc.stdin.flush()

        # Read 4-byte response length
        resp_len_b = proc.stdout.read(4)
        if not resp_len_b:
            raise RuntimeError("Port driver exited prematurely")
        resp_len = struct.unpack("!I", resp_len_b)[0]
        resp_data = proc.stdout.read(resp_len)
        status_tag = resp_data[0]
        payload = resp_data[1:]
        return status_tag, payload

    # 1. Connect
    status, payload = send_port_cmd(1, service_name.encode('utf-8'))
    assert status == 0, f"Connect failed: {payload.decode('utf-8')}"
    print(f"  -> Port connected to service '{service_name}': {payload.decode('utf-8')}")

    # 2. Synchronous Send: "SET cutoff 2200"
    status, payload = send_port_cmd(2, b"SET cutoff 2200\0")
    assert status == 0, f"Send failed: {payload.decode('utf-8')}"
    reply_str = payload.decode('utf-8').rstrip('\0')
    print(f"  -> Port synchronous send reply: '{reply_str}'")
    assert "OK cutoff=2200.0" in reply_str

    # 3. Synchronous Send: "GET cutoff"
    status, payload = send_port_cmd(2, b"GET cutoff\0")
    assert status == 0
    reply_str = payload.decode('utf-8').rstrip('\0')
    print(f"  -> Port synchronous send reply: '{reply_str}'")
    assert "VAL cutoff=2200.0" in reply_str

    # 4. Asynchronous Pulse: code=1, val=12
    pulse_bytes = struct.pack("<IQ", 1, 12)
    status, payload = send_port_cmd(3, pulse_bytes)
    assert status == 0
    print("  -> Port pulse sent: OK")

    # 5. Disconnect
    status, payload = send_port_cmd(4)
    assert status == 0
    proc.stdin.close()
    proc.stdout.close()
    proc.wait()
    print("  -> Port driver test PASSED.\n")

if __name__ == "__main__":
    test_dir = os.path.dirname(os.path.abspath(__file__))
    beam_dir = os.path.dirname(test_dir)
    adapters_dir = os.path.dirname(beam_dir)
    root_dir = os.path.dirname(adapters_dir)

    server_bin = os.path.join(root_dir, "examples", "synth_engine", "synth_server")
    port_bin = os.path.join(beam_dir, "srr_port_driver")
    service_name = "srr_beam_test_svc"
    sock_path = f"/tmp/srr_{service_name}.sock"

    # Start synth_server
    server_proc = subprocess.Popen([server_bin, service_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.15)

    try:
        test_direct_beam_wire_protocol(sock_path)
        test_beam_port_driver(port_bin, service_name)
        print("ALL BEAM ADAPTER TESTS COMPLETED SUCCESSFULLY!")
    finally:
        server_proc.kill()
        server_proc.wait()
        if os.path.exists(sock_path):
            os.remove(sock_path)
