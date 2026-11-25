#!/usr/bin/env python3
"""
SRR Python Client (Zero External Dependencies)
Communicates directly with SRR IPC services over Unix Domain Sockets
using only Python's standard library (`socket` and `struct`).
"""

import socket
import struct
import sys
import time
from typing import Optional

SRR_PROTO_MAGIC = 0x53525231  # "SRR1"
SRR_PROTO_VERSION = 1

SRR_MSG_REQUEST = 2
SRR_MSG_REPLY = 3
SRR_MSG_ERROR = 4
SRR_MSG_PULSE = 5
SRR_MSG_GOODBYE = 7

# Header format: uint32 magic, uint16 version, uint16 msg_type, uint32 flags, uint64 trans_id, uint32 payload_len
HEADER_FMT = "<IHHIQI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 24 bytes


class SRRClient:
    """Client for synchronous Send/Receive/Reply IPC services."""

    def __init__(self, service_name: str, socket_dir: str = "/tmp"):
        self.service_name = service_name
        self.socket_path = f"{socket_dir}/srr_{service_name}.sock"
        self._sock: Optional[socket.socket] = None
        self._next_trans_id = 1

    def connect(self, timeout: float = 2.0) -> None:
        """Connect to the Unix Domain Socket."""
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(self.socket_path)

    def close(self) -> None:
        """Close connection with orderly goodbye."""
        if self._sock:
            try:
                goodbye_hdr = struct.pack(
                    HEADER_FMT,
                    SRR_PROTO_MAGIC,
                    SRR_PROTO_VERSION,
                    SRR_MSG_GOODBYE,
                    0,
                    0,
                    0,
                )
                self._sock.sendall(goodbye_hdr)
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def send(self, payload: bytes, timeout: float = 2.0) -> bytes:
        """
        Synchronously send request payload and block until reply arrives.
        """
        if not self._sock:
            self.connect(timeout)

        assert self._sock is not None
        self._sock.settimeout(timeout)

        tid = self._next_trans_id
        self._next_trans_id += 1

        # 1. Send Request Frame (24-byte header + payload)
        hdr = struct.pack(
            HEADER_FMT,
            SRR_PROTO_MAGIC,
            SRR_PROTO_VERSION,
            SRR_MSG_REQUEST,
            0,
            tid,
            len(payload),
        )
        self._sock.sendall(hdr + payload)

        # 2. Receive Reply Header (24 bytes)
        raw_hdr = self._recv_exact(HEADER_SIZE)
        magic, ver, msg_type, flags, reply_tid, payload_len = struct.unpack(
            HEADER_FMT, raw_hdr
        )

        if magic != SRR_PROTO_MAGIC or ver != SRR_PROTO_VERSION:
            raise ValueError(f"Invalid SRR wire protocol magic/version: {hex(magic)}")

        # 3. Receive Payload
        reply_payload = self._recv_exact(payload_len) if payload_len > 0 else b""

        if msg_type == SRR_MSG_ERROR:
            raise RuntimeError(f"Server returned error code: {reply_payload}")
        if msg_type != SRR_MSG_REPLY:
            raise ValueError(f"Unexpected response message type: {msg_type}")

        return reply_payload

    def pulse(self, code: int, value: int) -> None:
        """
        Send an asynchronous one-way pulse.
        Pulse payload: uint32 code, uint32 reserved, uint64 value (16 bytes)
        """
        if not self._sock:
            self.connect()
        assert self._sock is not None

        pulse_body = struct.pack("<IIQ", code, 0, value)
        hdr = struct.pack(
            HEADER_FMT,
            SRR_PROTO_MAGIC,
            SRR_PROTO_VERSION,
            SRR_MSG_PULSE,
            0,
            0,
            len(pulse_body),
        )
        self._sock.sendall(hdr + pulse_body)

    def _recv_exact(self, num_bytes: int) -> bytes:
        """Read exactly num_bytes from socket."""
        assert self._sock is not None
        buf = bytearray()
        while len(buf) < num_bytes:
            chunk = self._sock.recv(num_bytes - len(buf))
            if not chunk:
                raise ConnectionResetError("SRR server disconnected unexpectedly.")
            buf.extend(chunk)
        return bytes(buf)

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


def demo():
    service_name = "srr_synth"
    print(f"Connecting to SRR service '{service_name}'...")

    try:
        with SRRClient(service_name) as client:
            # 1. Send status query
            t0 = time.perf_counter()
            reply = client.send(b"STATUS")
            t1 = time.perf_counter()
            print(f"[REPLY in {(t1-t0)*1e6:.1f} µs]: {reply.decode('utf-8', errors='replace')}")

            # 2. Update parameter
            reply = client.send(b"SET cutoff 2800.0")
            print(f"[REPLY]: {reply.decode('utf-8', errors='replace')}")

            # 3. Send pulse (e.g. clock tick or voice trigger)
            client.pulse(code=1, value=4)
            print("[PULSE]: Sent successfully.")

    except FileNotFoundError:
        print(f"Service socket not found at /tmp/srr_{service_name}.sock.")
        print("Start the synthesizer server first with: ./examples/synth_engine/synth_server")
        sys.exit(0)
    except ConnectionRefusedError:
        print(f"Connection refused on /tmp/srr_{service_name}.sock.")
        sys.exit(0)


if __name__ == "__main__":
    demo()
