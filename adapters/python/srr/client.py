"""
SRR Clients: Unix Domain Socket IPC, UDP Datagram, and Resolver Discovery.
"""

import os
import socket
import struct
import time
from typing import Optional, Union, Tuple, Dict, Any

from .protocol import (
    SRR_WIRE_MAGIC,
    SRR_WIRE_VERSION,
    SRR_MSG_REQUEST,
    SRR_MSG_REPLY,
    SRR_MSG_ERROR,
    SRR_MSG_PULSE,
    IPC_HEADER_SIZE,
    encode_ipc_request,
    encode_ipc_pulse,
    encode_ipc_goodbye,
    decode_ipc_header,
    pack_udp_datagram,
    unpack_udp_datagram,
)


class SRRError(Exception):
    """Raised when an SRR server returns an explicit error or protocol fault."""
    pass


class SRRClient:
    """
    Synchronous Send/Receive/Reply client over Unix Domain Sockets.
    """

    def __init__(self, service_name: str, socket_dir: str = "/tmp"):
        self.service_name = service_name
        self.socket_path = (
            service_name if service_name.startswith("/")
            else f"{socket_dir}/srr_{service_name}.sock"
        )
        self._sock: Optional[socket.socket] = None
        self._next_trans_id = 1

    def connect(self, timeout: float = 2.0) -> "SRRClient":
        """Connects to the service Unix Domain Socket."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect(self.socket_path)
        self._sock = sock
        return self

    def close(self) -> None:
        """Sends goodbye and terminates the connection."""
        if self._sock:
            try:
                self._sock.sendall(encode_ipc_goodbye())
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "SRRClient":
        return self.connect()

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def _recv_exact(self, count: int) -> bytes:
        buf = bytearray()
        while len(buf) < count:
            chunk = self._sock.recv(count - len(buf))
            if not chunk:
                raise ConnectionResetError("Connection closed unexpectedly by remote peer")
            buf.extend(chunk)
        return bytes(buf)

    def call(self, payload: Union[str, bytes], timeout: float = 2.0) -> bytes:
        """
        Sends request payload, blocks caller until reply is received.
        """
        if not self._sock:
            self.connect(timeout)

        assert self._sock is not None
        self._sock.settimeout(timeout)

        if isinstance(payload, str):
            payload_bytes = payload.encode("utf-8")
        else:
            payload_bytes = bytes(payload)

        tid = self._next_trans_id
        self._next_trans_id += 1

        frame = encode_ipc_request(tid, payload_bytes)
        self._sock.sendall(frame)

        raw_hdr = self._recv_exact(IPC_HEADER_SIZE)
        hdr = decode_ipc_header(raw_hdr)

        reply_payload = self._recv_exact(hdr["payload_len"]) if hdr["payload_len"] > 0 else b""

        if hdr["msg_type"] == SRR_MSG_ERROR:
            raise SRRError(f"Remote service returned error: {reply_payload.decode('utf-8', errors='replace')}")

        return reply_payload

    def pulse(self, code: int, value: int) -> None:
        """
        Emits an asynchronous one-way pulse (non-blocking notification).
        """
        if not self._sock:
            self.connect()
        assert self._sock is not None
        frame = encode_ipc_pulse(code, value)
        self._sock.sendall(frame)


class SRRUDPClient:
    """
    Synchronous Send/Receive/Reply client over UDP.
    """

    def __init__(self, host: str, port: int, default_timeout: float = 1.0):
        self.host = host
        self.port = port
        self.default_timeout = default_timeout
        self.session_id = int(time.time()) & 0xFFFFFFFF
        self.next_trans_id = 1
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(default_timeout)

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None

    def __enter__(self) -> "SRRUDPClient":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def call(
        self,
        service_id: int,
        operation_id: int,
        payload: Union[str, bytes] = b"",
        timeout: Optional[float] = None,
    ) -> bytes:
        """
        Sends a request datagram to the remote endpoint and waits for matching reply.
        """
        if isinstance(payload, str):
            payload_bytes = payload.encode("utf-8")
        else:
            payload_bytes = bytes(payload)

        tid = self.next_trans_id
        self.next_trans_id += 1

        dgram = pack_udp_datagram(
            msg_type=SRR_MSG_REQUEST,
            service_id=service_id,
            operation_id=operation_id,
            session_id=self.session_id,
            trans_id=tid,
            payload=payload_bytes,
        )

        effective_timeout = timeout if timeout is not None else self.default_timeout
        self._sock.settimeout(effective_timeout)
        self._sock.sendto(dgram, (self.host, self.port))

        start_time = time.time()
        while True:
            remaining = effective_timeout - (time.time() - start_time)
            if remaining <= 0:
                raise TimeoutError("Timed out waiting for SRR UDP reply")

            self._sock.settimeout(remaining)
            resp_data, _peer = self._sock.recvfrom(65535)

            try:
                hdr, reply_payload = unpack_udp_datagram(resp_data)
            except ValueError:
                continue  # ignore malformed or corrupted datagrams

            if hdr["trans_id"] == tid:
                if hdr["msg_type"] == SRR_MSG_ERROR:
                    raise SRRError(f"UDP service returned error: {reply_payload.decode('utf-8', errors='replace')}")
                return reply_payload

    def pulse(self, service_id: int, operation_id: int, code: int, value: int) -> None:
        """Emits an asynchronous UDP pulse."""
        body = struct.pack("<IIQ", code, 0, value)
        dgram = pack_udp_datagram(
            msg_type=SRR_MSG_PULSE,
            service_id=service_id,
            operation_id=operation_id,
            session_id=self.session_id,
            trans_id=0,
            payload=body,
        )
        self._sock.sendto(dgram, (self.host, self.port))


class SRRResolver:
    """
    Client helper to resolve service names into transport endpoints
    via the SRR Resolver daemon (/tmp/srr_resolver.sock or UDP:8889).
    """

    @staticmethod
    def resolve_ipc(service_name: str) -> Optional[str]:
        """
        Queries local resolver for a service name. Returns socket path or None.
        """
        resolver_sock = "/tmp/srr_resolver.sock"
        if not os.path.exists(resolver_sock):
            # Fallback direct socket
            direct = f"/tmp/srr_{service_name}.sock"
            return direct if os.path.exists(direct) else None

        try:
            client = SRRClient("/tmp/srr_resolver.sock")
            with client:
                res = client.call(f"RESOLVE {service_name}").decode("utf-8")
                # Format: OK <type> <endpoint>
                parts = res.strip().split()
                if len(parts) >= 3 and parts[0] == "OK":
                    return parts[2]
        except Exception:
            pass
        return None
