"""
Wire protocol encoding, decoding, and integrity verification for SRR.
"""

import struct
from typing import Dict, Any, Tuple, Optional

SRR_WIRE_MAGIC = 0x53525231  # "SRR1"
SRR_WIRE_VERSION = 1

SRR_MSG_HELLO = 1
SRR_MSG_REQUEST = 2
SRR_MSG_REPLY = 3
SRR_MSG_ERROR = 4
SRR_MSG_PULSE = 5
SRR_MSG_CANCEL = 6
SRR_MSG_GOODBYE = 7

# IPC stream header (24 bytes Little-Endian):
# uint32 magic, uint16 version, uint16 msg_type, uint32 flags, uint64 trans_id, uint32 payload_len
IPC_HEADER_FMT = "<IHHIQI"
IPC_HEADER_SIZE = struct.calcsize(IPC_HEADER_FMT)  # 24

# UDP / Wire header (32 bytes Little-Endian):
# uint32 magic, uint16 version, uint16 msg_type, uint16 flags, uint16 service_id,
# uint16 operation_id, uint16 reserved, uint32 session_id, uint64 trans_id, uint32 payload_len
WIRE_HEADER_FMT = "<IHHHHHHIQI"
WIRE_HEADER_SIZE = struct.calcsize(WIRE_HEADER_FMT)  # 32
CRC_SIZE = 2


def srr_crc16(data: bytes, init_crc: int = 0xFFFF) -> int:
    """
    CRC-16-CCITT (False): Poly 0x1021, Init 0xFFFF, no reflect.
    Matches C implementation: srr_crc16("123456789") == 0x29B1.
    """
    crc = init_crc & 0xFFFF
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """Consistent Overhead Byte Stuffing (COBS) encoder."""
    out = bytearray([1])
    code_idx = 0
    code = 1
    for b in data:
        if b == 0:
            out[code_idx] = code
            code = 1
            code_idx = len(out)
            out.append(1)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code = 1
                code_idx = len(out)
                out.append(1)
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Consistent Overhead Byte Stuffing (COBS) decoder."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        code = data[i]
        i += 1
        if code == 0:
            raise ValueError("Zero byte encountered inside COBS data")
        for _ in range(1, code):
            if i >= n:
                raise ValueError("Truncated COBS stream")
            out.append(data[i])
            i += 1
        if code < 0xFF and i < n:
            out.append(0)
    return bytes(out)


def encode_wire_frame(
    msg_type: int,
    service_id: int,
    operation_id: int,
    session_id: int,
    trans_id: int,
    payload: bytes = b"",
    flags: int = 0,
) -> bytes:
    """Encodes a complete COBS-framed wire packet terminated by 0x00 delimiter."""
    hdr = struct.pack(
        WIRE_HEADER_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        msg_type,
        flags,
        service_id,
        operation_id,
        0,  # reserved
        session_id,
        trans_id,
        len(payload),
    )
    raw = hdr + payload
    crc = srr_crc16(raw, 0xFFFF)
    return cobs_encode(raw + struct.pack("<H", crc)) + b"\x00"


def decode_wire_frame(frame: bytes) -> Tuple[Dict[str, Any], bytes]:
    """Decodes a COBS-framed wire packet terminated by 0x00 delimiter."""
    if not frame or frame[-1:] != b"\x00":
        raise ValueError("Frame must end with 0x00 delimiter")
    un_cobs = cobs_decode(frame[:-1])
    min_len = WIRE_HEADER_SIZE + CRC_SIZE
    if len(un_cobs) < min_len:
        raise ValueError(f"Decoded wire frame too short: {len(un_cobs)} bytes")
    body = un_cobs[:-CRC_SIZE]
    received_crc = struct.unpack("<H", un_cobs[-CRC_SIZE:])[0]
    calc_crc = srr_crc16(body, 0xFFFF)
    if received_crc != calc_crc:
        raise ValueError(f"CRC-16 mismatch: expected {hex(calc_crc)}, got {hex(received_crc)}")
    fields = struct.unpack(WIRE_HEADER_FMT, body[:WIRE_HEADER_SIZE])
    magic, ver, msg_type, flags, s_id, op_id, res, sess_id, tid, plen = fields
    if magic != SRR_WIRE_MAGIC or ver != SRR_WIRE_VERSION:
        raise ValueError("Invalid magic or version in wire header")
    payload = body[WIRE_HEADER_SIZE:]
    if len(payload) != plen:
        raise ValueError(f"Payload length mismatch: header says {plen}, got {len(payload)}")
    hdr_dict = {
        "magic": magic,
        "version": ver,
        "msg_type": msg_type,
        "flags": flags,
        "service_id": s_id,
        "operation_id": op_id,
        "session_id": sess_id,
        "trans_id": tid,
        "payload_len": plen,
    }
    return hdr_dict, payload


def encode_ipc_request(trans_id: int, payload: bytes) -> bytes:
    """Encodes a synchronous request frame for Unix Domain Sockets."""
    hdr = struct.pack(
        IPC_HEADER_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        SRR_MSG_REQUEST,
        0,
        trans_id,
        len(payload),
    )
    return hdr + payload


def encode_ipc_pulse(code: int, value: int) -> bytes:
    """Encodes an asynchronous one-way pulse frame for Unix Domain Sockets."""
    body = struct.pack("<IIQ", code, 0, value)
    hdr = struct.pack(
        IPC_HEADER_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        SRR_MSG_PULSE,
        0,
        0,
        len(body),
    )
    return hdr + body


def encode_ipc_goodbye() -> bytes:
    """Encodes an orderly disconnect frame for Unix Domain Sockets."""
    return struct.pack(
        IPC_HEADER_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        SRR_MSG_GOODBYE,
        0,
        0,
        0,
    )


def decode_ipc_header(raw_hdr: bytes) -> Dict[str, Any]:
    """Decodes a 24-byte IPC frame header."""
    if len(raw_hdr) < IPC_HEADER_SIZE:
        raise ValueError(f"IPC header truncated: expected {IPC_HEADER_SIZE} bytes, got {len(raw_hdr)}")
    magic, ver, msg_type, flags, trans_id, payload_len = struct.unpack(IPC_HEADER_FMT, raw_hdr[:IPC_HEADER_SIZE])
    if magic != SRR_WIRE_MAGIC:
        raise ValueError(f"Invalid SRR magic: {hex(magic)}")
    if ver != SRR_WIRE_VERSION:
        raise ValueError(f"Unsupported SRR protocol version: {ver}")
    return {
        "magic": magic,
        "version": ver,
        "msg_type": msg_type,
        "flags": flags,
        "trans_id": trans_id,
        "payload_len": payload_len,
    }


def pack_udp_datagram(
    msg_type: int,
    service_id: int,
    operation_id: int,
    session_id: int,
    trans_id: int,
    payload: bytes = b"",
) -> bytes:
    """
    Packs an SRR datagram: [32B Header] + [Payload] + [2B CRC16-LE]
    """
    hdr = struct.pack(
        WIRE_HEADER_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        msg_type,
        0,  # flags
        service_id,
        operation_id,
        0,  # reserved
        session_id,
        trans_id,
        len(payload),
    )
    frame_body = hdr + payload
    crc = srr_crc16(frame_body, 0xFFFF)
    return frame_body + struct.pack("<H", crc)


def unpack_udp_datagram(dgram: bytes) -> Tuple[Dict[str, Any], bytes]:
    """
    Unpacks and verifies an SRR datagram: returns (header_dict, payload_bytes).
    """
    min_len = WIRE_HEADER_SIZE + CRC_SIZE
    if len(dgram) < min_len:
        raise ValueError(f"Datagram too short: {len(dgram)} bytes")

    body = dgram[:-CRC_SIZE]
    received_crc = struct.unpack("<H", dgram[-CRC_SIZE:])[0]
    calc_crc = srr_crc16(body, 0xFFFF)
    if received_crc != calc_crc:
        raise ValueError(f"CRC-16 mismatch: expected {hex(calc_crc)}, got {hex(received_crc)}")

    fields = struct.unpack(WIRE_HEADER_FMT, body[:WIRE_HEADER_SIZE])
    magic, ver, msg_type, flags, s_id, op_id, res, sess_id, tid, plen = fields

    if magic != SRR_WIRE_MAGIC or ver != SRR_WIRE_VERSION:
        raise ValueError("Invalid magic or version in wire header")

    payload = body[WIRE_HEADER_SIZE:]
    if len(payload) != plen:
        raise ValueError(f"Payload length mismatch: header says {plen}, got {len(payload)}")

    hdr_dict = {
        "magic": magic,
        "version": ver,
        "msg_type": msg_type,
        "flags": flags,
        "service_id": s_id,
        "operation_id": op_id,
        "session_id": sess_id,
        "trans_id": tid,
        "payload_len": plen,
    }
    return hdr_dict, payload
