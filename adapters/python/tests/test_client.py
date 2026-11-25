#!/usr/bin/env python3
"""
Unit and integration tests for the Python SRR native adapter.
"""

import os
import sys
import unittest

# Ensure adapters/python is in sys.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from srr.protocol import (
    srr_crc16,
    encode_ipc_request,
    encode_ipc_pulse,
    decode_ipc_header,
    pack_udp_datagram,
    unpack_udp_datagram,
    SRR_WIRE_MAGIC,
    SRR_WIRE_VERSION,
    SRR_MSG_REQUEST,
    SRR_MSG_PULSE,
)
from srr.client import SRRClient, SRRUDPClient


class TestSRRProtocol(unittest.TestCase):
    def test_crc16_standard_vector(self):
        # Standard CCITT-False vector "123456789" -> 0x29B1
        self.assertEqual(srr_crc16(b"123456789"), 0x29B1)

    def test_ipc_framing(self):
        req_frame = encode_ipc_request(trans_id=42, payload=b"HELLO_WORLD")
        self.assertEqual(len(req_frame), 24 + 11)

        hdr = decode_ipc_header(req_frame[:24])
        self.assertEqual(hdr["magic"], SRR_WIRE_MAGIC)
        self.assertEqual(hdr["version"], SRR_WIRE_VERSION)
        self.assertEqual(hdr["msg_type"], SRR_MSG_REQUEST)
        self.assertEqual(hdr["trans_id"], 42)
        self.assertEqual(hdr["payload_len"], 11)
        self.assertEqual(req_frame[24:], b"HELLO_WORLD")

    def test_ipc_pulse_framing(self):
        pulse_frame = encode_ipc_pulse(code=5, value=12345678)
        self.assertEqual(len(pulse_frame), 24 + 16)
        hdr = decode_ipc_header(pulse_frame[:24])
        self.assertEqual(hdr["msg_type"], SRR_MSG_PULSE)
        self.assertEqual(hdr["payload_len"], 16)

    def test_udp_datagram_roundtrip(self):
        dgram = pack_udp_datagram(
            msg_type=SRR_MSG_REQUEST,
            service_id=10,
            operation_id=20,
            session_id=1234,
            trans_id=99,
            payload=b"PING_PAYLOAD",
        )
        self.assertEqual(len(dgram), 32 + 12 + 2)

        hdr, payload = unpack_udp_datagram(dgram)
        self.assertEqual(hdr["service_id"], 10)
        self.assertEqual(hdr["operation_id"], 20)
        self.assertEqual(hdr["session_id"], 1234)
        self.assertEqual(hdr["trans_id"], 99)
        self.assertEqual(payload, b"PING_PAYLOAD")

    def test_udp_corrupted_crc_rejected(self):
        dgram = bytearray(pack_udp_datagram(
            msg_type=SRR_MSG_REQUEST,
            service_id=1,
            operation_id=1,
            session_id=1,
            trans_id=1,
            payload=b"DATA",
        ))
        # Corrupt one payload byte
        dgram[33] ^= 0xFF
        with self.assertRaises(ValueError):
            unpack_udp_datagram(bytes(dgram))


if __name__ == "__main__":
    unittest.main()
