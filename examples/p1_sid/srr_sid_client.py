#!/usr/bin/env python3
"""
srr_sid_client.py - SRR Parallax Propeller P1 SIDcog Client

Controls the MOS 6581 SID sound synthesizer running on a Parallax Propeller P1
via high-speed serial (UART) using standard SRR wire framing (COBS + CRC-16).
"""

import sys
import time
import struct
import argparse

SRR_MAGIC = 0x53525231
SRR_VERSION = 1

SRR_WIRE_REQUEST = 2
SRR_WIRE_REPLY   = 3
SRR_WIRE_PULSE   = 5

SRR_SVC_SID = 0x0051

OP_WRITE_REG   = 0x0001
OP_WRITE_BATCH = 0x0002
OP_WRITE_DUMP  = 0x0003
OP_RESET       = 0x0004
OP_READ_REGS   = 0x0005

# SID Registers
SID_V1_FREQ_LO = 0x00
SID_V1_FREQ_HI = 0x01
SID_V1_PW_LO   = 0x02
SID_V1_PW_HI   = 0x03
SID_V1_CR      = 0x04
SID_V1_AD      = 0x05
SID_V1_SR      = 0x06
SID_FC_LO      = 0x15
SID_FC_HI      = 0x16
SID_RES_FILT   = 0x17
SID_MODE_VOL   = 0x18

CR_GATE  = 0x01
CR_SYNC  = 0x02
CR_RING  = 0x04
CR_TEST  = 0x08
CR_TRI   = 0x10
CR_SAW   = 0x20
CR_PULSE = 0x40
CR_NOISE = 0x80

FILT_LP  = 0x10
FILT_BP  = 0x20
FILT_HP  = 0x40

def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

def cobs_encode(src: bytes) -> bytes:
    out = bytearray()
    code_idx = 0
    out.append(1)
    code = 1
    for b in src:
        if b == 0:
            out[code_idx] = code
            code_idx = len(out)
            out.append(1)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx = len(out)
                out.append(1)
                code = 1
    out[code_idx] = code
    out.append(0)  # Trailing zero delimiter
    return bytes(out)

def cobs_decode(src: bytes) -> bytes:
    out = bytearray()
    idx = 0
    n = len(src)
    while idx < n:
        code = src[idx]
        idx += 1
        if code == 0:
            break
        for _ in range(code - 1):
            if idx >= n:
                break
            out.append(src[idx])
            idx += 1
        if code < 0xFF and idx < n and src[idx] != 0:
            out.append(0)
    return bytes(out)

def build_frame(msg_type: int, service_id: int, operation_id: int, trans_id: int, payload: bytes) -> bytes:
    hdr = struct.pack(
        "<IH4HIIQ",
        SRR_MAGIC,
        SRR_VERSION,
        msg_type,
        0,  # flags
        service_id,
        operation_id,
        0,  # reserved
        0,  # session_id
        trans_id
    ) + struct.pack("<I", len(payload))
    raw = hdr + payload
    crc = crc16_ccitt(raw)
    raw += struct.pack("<H", crc)
    return cobs_encode(raw)

class SrrSidClient:
    def __init__(self, port_device: str, baudrate: int = 115200, chip: int = 0):
        try:
            import serial
            self.ser = serial.Serial(port_device, baudrate, timeout=1.0)
        except ImportError:
            sys.stderr.write("Note: 'pyserial' not installed. Running in mock/dry-run mode.\n")
            self.ser = None
        self.trans_id = 1
        self.chip = chip

    def send_request(self, op: int, payload: bytes, chip: int = None) -> bytes:
        if chip is None:
            chip = self.chip
        svc = 0x0050 + chip
        tid = self.trans_id
        self.trans_id += 1
        pkt = build_frame(SRR_WIRE_REQUEST, svc, op, tid, payload)
        if self.ser:
            self.ser.write(pkt)
            raw = self.ser.read_until(b'\x00')
            if raw and raw.endswith(b'\x00'):
                dec = cobs_decode(raw[:-1])
                if len(dec) >= 34:
                    return dec[32:-2]
        return b'\x00'

    def send_pulse(self, reg: int, val: int, chip: int = None):
        if chip is None:
            chip = self.chip
        svc = 0x0050 + chip
        pkt = build_frame(SRR_WIRE_PULSE, svc, reg, 0, bytes([reg, val]))
        if self.ser:
            self.ser.write(pkt)

    def write_reg(self, reg: int, val: int, chip: int = None):
        return self.send_request(OP_WRITE_REG, bytes([reg, val]), chip)

    def write_batch(self, pairs: list, chip: int = None):
        payload = bytearray()
        for r, v in pairs:
            payload.extend([r, v])
        return self.send_request(OP_WRITE_BATCH, bytes(payload), chip)

    def reset(self, chip: int = None):
        return self.send_request(OP_RESET, b'', chip)

    def play_arpeggio(self):
        print(f"Playing C minor arpeggio lead on SID #{self.chip}...")
        self.reset()
        self.write_batch([
            (SID_MODE_VOL, 0x0F),
            (SID_V1_AD, 0x14),
            (SID_V1_SR, 0xC4),
            (SID_V1_CR, CR_SAW | CR_GATE)
        ])
        notes = [4494, 5345, 6356, 7558, 8989, 7558, 6356, 5345]
        for _ in range(6):
            for f in notes:
                self.send_pulse(SID_V1_FREQ_LO, f & 0xFF)
                self.send_pulse(SID_V1_FREQ_HI, (f >> 8) & 0xFF)
                time.sleep(0.04)
        self.reset()
        print("Arpeggio done.")

    def play_stereo(self):
        print("Playing Dual-SID Stereo Chorus (SID 0 = Left, SID 1 = Right)...")
        self.reset(chip=0)
        self.reset(chip=1)
        self.write_batch([(SID_MODE_VOL, 0x0F), (SID_V1_AD, 0x14), (SID_V1_SR, 0xC8), (SID_V1_CR, CR_SAW | CR_GATE)], chip=0)
        self.write_batch([(SID_MODE_VOL, 0x0F), (SID_V1_AD, 0x24), (SID_V1_SR, 0xB8), (SID_V1_CR, CR_SAW | CR_GATE)], chip=1)
        notes = [4494, 5345, 6356, 7558, 8989, 7558, 6356, 5345]
        for _ in range(8):
            for f in notes:
                f_left = f
                f_right = f + 18  # stereo chorus detune
                self.send_pulse(SID_V1_FREQ_LO, f_left & 0xFF, chip=0)
                self.send_pulse(SID_V1_FREQ_HI, (f_left >> 8) & 0xFF, chip=0)
                self.send_pulse(SID_V1_FREQ_LO, f_right & 0xFF, chip=1)
                self.send_pulse(SID_V1_FREQ_HI, (f_right >> 8) & 0xFF, chip=1)
                time.sleep(0.035)
        self.reset(chip=0)
        self.reset(chip=1)
        print("Stereo chorus done.")

def main():
    parser = argparse.ArgumentParser(description="SRR Parallax Propeller P1 Multi-SID Client")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--chip", type=int, default=0, help="Target SID chip (0..5)")
    parser.add_argument("command", choices=["arp", "stereo", "reset", "set"], default="stereo", nargs="?")
    parser.add_argument("args", nargs="*", help="Optional arguments for command")

    args = parser.parse_args()
    client = SrrSidClient(args.port, args.baud, args.chip)

    if args.command == "arp":
        client.play_arpeggio()
    elif args.command == "stereo":
        client.play_stereo()
    elif args.command == "reset":
        client.reset()
        print(f"SID #{args.chip} reset.")
    elif args.command == "set" and len(args.args) >= 2:
        reg = int(args.args[0], 0)
        val = int(args.args[1], 0)
        client.write_reg(reg, val)
        print(f"SID #{args.chip} register 0x{reg:02X} set to 0x{val:02X}")

if __name__ == "__main__":
    main()
