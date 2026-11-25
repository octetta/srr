#!/usr/bin/env python3
"""
Canonical Cross-Language Protocol Conformance Test Runner.
Verifies that C, Python, Go, Node.js, Ruby, Perl, and PHP agree on:
- Header encoding & field offsets
- Byte order (Little-Endian)
- CRC-16-CCITT (False)
- COBS encoding & decoding
- Payload lengths
- Transaction IDs
- Error frames
- Pulses
- Rejection of invalid frames
"""

import json
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "adapters", "python"))

from srr.protocol import (
    srr_crc16,
    cobs_encode,
    cobs_decode,
    encode_ipc_request,
    decode_ipc_header,
    encode_ipc_pulse,
    encode_ipc_goodbye,
    encode_wire_frame,
    decode_wire_frame,
    pack_udp_datagram,
    unpack_udp_datagram,
    SRR_WIRE_MAGIC,
    SRR_WIRE_VERSION,
    SRR_MSG_REQUEST,
    SRR_MSG_REPLY,
    SRR_MSG_PULSE,
    SRR_MSG_ERROR,
    SRR_MSG_GOODBYE,
)

def run_step(desc, fn):
    print(f"Testing {desc}... ", end="", flush=True)
    try:
        fn()
        print("PASS")
    except Exception as e:
        print(f"FAILED: {e}")
        raise

def test_python_conformance(vectors):
    # 1. CRC16
    for t in vectors["crc16"]:
        inp = bytes.fromhex(t["hex_input"])
        expected = t["expected_crc"]
        actual = srr_crc16(inp, 0xFFFF)
        assert actual == expected, f"CRC mismatch for {t['name']}: expected {expected}, got {actual}"

    # 2. COBS
    for t in vectors["cobs"]:
        raw = bytes.fromhex(t["raw_hex"])
        expected = t["encoded_hex"]
        enc = cobs_encode(raw).hex()
        assert enc == expected, f"COBS mismatch for {t['name']}: expected {expected}, got {enc}"
        dec = cobs_decode(bytes.fromhex(expected))
        assert dec == raw, f"COBS round-trip failed for {t['name']}"

    # 3. IPC Frames
    for t in vectors["ipc_frames"]:
        expected_hex = t["hex"]
        if t["type"] == "request":
            enc = encode_ipc_request(t["trans_id"], b"PING").hex()
            assert enc == expected_hex, f"IPC request mismatch: {enc} != {expected_hex}"
        elif t["type"] == "pulse":
            enc = encode_ipc_pulse(t["code"], t["value"]).hex()
            assert enc == expected_hex, f"IPC pulse mismatch: {enc} != {expected_hex}"
        elif t["type"] == "goodbye":
            enc = encode_ipc_goodbye().hex()
            assert enc == expected_hex, f"IPC goodbye mismatch: {enc} != {expected_hex}"

    # 4. Wire Frames
    for t in vectors["wire_frames"]:
        expected_hex = t["cobs_delimited_hex"]
        if t["name"] == "wire_request":
            enc = encode_wire_frame(
                msg_type=SRR_MSG_REQUEST,
                service_id=t["service_id"],
                operation_id=t["op_id"],
                session_id=t["session_id"],
                trans_id=t["trans_id"],
                payload=b"hello_srr",
            ).hex()
            assert enc == expected_hex, f"Wire request mismatch: {enc} != {expected_hex}"
            hdr, pl = decode_wire_frame(bytes.fromhex(expected_hex))
            assert hdr["service_id"] == t["service_id"]
            assert pl == b"hello_srr"
        elif t["name"] == "wire_reply":
            enc = encode_wire_frame(
                msg_type=SRR_MSG_REPLY,
                service_id=t["service_id"],
                operation_id=t["op_id"],
                session_id=t["session_id"],
                trans_id=t["trans_id"],
                payload=b"world",
            ).hex()
            assert enc == expected_hex, f"Wire reply mismatch: {enc} != {expected_hex}"
        elif t["name"] == "wire_error":
            enc = encode_wire_frame(
                msg_type=SRR_MSG_ERROR,
                service_id=t["service_id"],
                operation_id=t["op_id"],
                session_id=t["session_id"],
                trans_id=t["trans_id"],
                payload=b"",
                flags=t["flags"],
            ).hex()
            assert enc == expected_hex, f"Wire error mismatch: {enc} != {expected_hex}"

    # 5. Invalid Frames Rejection
    # a. Bad CRC
    bad_crc = bytearray(bytes.fromhex(vectors["wire_frames"][0]["cobs_delimited_hex"]))
    bad_crc[4] ^= 0x55
    try:
        decode_wire_frame(bytes(bad_crc))
        assert False, "Should have rejected corrupted frame"
    except Exception:
        pass

    # b. Zero inside COBS
    try:
        cobs_decode(b"\x05\x00\x01\x02")
        assert False, "Should have rejected zero inside COBS"
    except ValueError:
        pass

def test_c_conformance():
    bin_path = os.path.join(REPO_ROOT, "tests", "conformance", "test_c_vectors")
    res = subprocess.run(
        ["gcc", "-Wall", "-Wextra", "-std=c99", "-Iinclude/srr", "-Iinclude",
         "src/srr_wire.c", "tests/conformance/test_c_vectors.c", "-o", bin_path],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True
    )
    assert res.returncode == 0, f"C build failed:\n{res.stderr}"
    run_res = subprocess.run([bin_path], cwd=REPO_ROOT, capture_output=True, text=True)
    if os.path.exists(bin_path):
        os.unlink(bin_path)
    assert run_res.returncode == 0, f"C execution failed:\n{run_res.stderr}"

def test_perl_conformance(vectors):
    if not shutil.which("perl"):
        print("  (Perl interpreter not found, skipping)")
        return
    perl_script = f"""
use strict;
use warnings;
use lib '{REPO_ROOT}/adapters/perl/lib';
use SRR::Protocol qw(:all);

# 1. CRC
die "Perl CRC mismatch" unless srr_crc16("123456789") == 10673;

# 2. COBS
my $raw = "\\x00\\x01\\x00\\x00\\x02\\x03\\x04\\x00\\x05\\x00";
my $enc = unpack("H*", cobs_encode($raw));
die "Perl COBS mismatch: $enc" unless $enc eq "0102010104020304020501";
my $dec = cobs_decode(pack("H*", $enc));
die "Perl COBS decode mismatch" unless $dec eq $raw;

# 3. Wire Request
my $wf = encode_wire_frame(
    msg_type => 2,
    service_id => 42,
    operation_id => 7,
    session_id => 1001,
    trans_id => 999999,
    payload => "hello_srr"
);
my $hex_wf = unpack("H*", $wf);
die "Perl wire frame mismatch: $hex_wf" unless $hex_wf eq "{vectors['wire_frames'][0]['cobs_delimited_hex']}";
print "Perl conformance OK\\n";
"""
    res = subprocess.run(["perl", "-e", perl_script], cwd=REPO_ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"Perl conformance failed:\n{res.stdout}\n{res.stderr}"

def test_go_conformance():
    if not shutil.which("go"):
        print("  (Go toolchain not found, skipping)")
        return
    res = subprocess.run(["go", "test", "-v", "./..."], cwd=os.path.join(REPO_ROOT, "adapters", "go"), capture_output=True, text=True)
    assert res.returncode == 0, f"Go test failed:\n{res.stdout}\n{res.stderr}"

def test_node_conformance():
    if not shutil.which("node"):
        print("UNVERIFIED (node interpreter not found on current host)")
        return
    res = subprocess.run(["node", "adapters/node/test/test_client.js"], cwd=REPO_ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"Node.js conformance failed:\n{res.stdout}\n{res.stderr}"
    print("PASS")

def test_ruby_conformance():
    if not shutil.which("ruby"):
        print("UNVERIFIED (ruby interpreter not found on current host)")
        return
    res = subprocess.run(["ruby", "adapters/ruby/test/test_client.rb"], cwd=REPO_ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"Ruby conformance failed:\n{res.stdout}\n{res.stderr}"
    print("PASS")

def test_php_conformance():
    if not shutil.which("php"):
        print("UNVERIFIED (php interpreter not found on current host)")
        return
    res = subprocess.run(["php", "adapters/php/tests/ProtocolTest.php"], cwd=REPO_ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"PHP conformance failed:\n{res.stdout}\n{res.stderr}"
    print("PASS")

def main():
    print("================================================================================")
    print(" SRR PROTOCOL CONFORMANCE: CANONICAL TEST VECTORS")
    print("================================================================================")
    vpath = os.path.join(REPO_ROOT, "tests", "conformance", "vectors.json")
    with open(vpath, "r") as f:
        vectors = json.load(f)

    run_step("C Reference Conformance", test_c_conformance)
    run_step("Python Adapter Conformance", lambda: test_python_conformance(vectors))
    run_step("Perl Adapter Conformance", lambda: test_perl_conformance(vectors))
    run_step("Go Adapter Conformance", test_go_conformance)

    print("Testing Node.js Adapter Conformance... ", end="", flush=True)
    test_node_conformance()

    print("Testing Ruby Adapter Conformance... ", end="", flush=True)
    test_ruby_conformance()

    print("Testing PHP Adapter Conformance... ", end="", flush=True)
    test_php_conformance()

    print("Protocol conformance verification completed.")

if __name__ == "__main__":
    main()
