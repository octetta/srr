'use strict';

/**
 * SRR Wire & IPC Protocol Constants and Framing Functions
 * Zero external dependencies; uses Node.js standard Buffer.
 */

const SRR_WIRE_MAGIC = 0x53525231; // "SRR1"
const SRR_WIRE_VERSION = 1;

const SRR_MSG_HELLO = 1;
const SRR_MSG_REQUEST = 2;
const SRR_MSG_REPLY = 3;
const SRR_MSG_ERROR = 4;
const SRR_MSG_PULSE = 5;
const SRR_MSG_CANCEL = 6;
const SRR_MSG_GOODBYE = 7;

const IPC_HEADER_SIZE = 24;
const WIRE_HEADER_SIZE = 32;
const CRC_SIZE = 2;

/**
 * CRC-16-CCITT (False): Poly 0x1021, Init 0xFFFF, no reflect.
 * Vector "123456789" -> 0x29B1.
 */
function srrCrc16(buf, initCrc = 0xFFFF) {
  let crc = initCrc & 0xFFFF;
  for (let i = 0; i < buf.length; i++) {
    crc ^= (buf[i] << 8) & 0xFFFF;
    for (let b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
      } else {
        crc = (crc << 1) & 0xFFFF;
      }
    }
  }
  return crc;
}

/**
 * Encodes a 24-byte IPC request frame.
 */
function encodeIpcRequest(transId, payload) {
  const payloadBuf = Buffer.isBuffer(payload) ? payload : Buffer.from(payload || '');
  const buf = Buffer.alloc(IPC_HEADER_SIZE + payloadBuf.length);

  buf.writeUInt32LE(SRR_WIRE_MAGIC, 0);
  buf.writeUInt16LE(SRR_WIRE_VERSION, 4);
  buf.writeUInt16LE(SRR_MSG_REQUEST, 6);
  buf.writeUInt32LE(0, 8); // flags
  buf.writeBigUInt64LE(BigInt(transId), 12);
  buf.writeUInt32LE(payloadBuf.length, 20);

  if (payloadBuf.length > 0) {
    payloadBuf.copy(buf, IPC_HEADER_SIZE);
  }
  return buf;
}

/**
 * Encodes an asynchronous one-way pulse frame (24-byte header + 16-byte body).
 */
function encodeIpcPulse(code, value) {
  const buf = Buffer.alloc(IPC_HEADER_SIZE + 16);

  buf.writeUInt32LE(SRR_WIRE_MAGIC, 0);
  buf.writeUInt16LE(SRR_WIRE_VERSION, 4);
  buf.writeUInt16LE(SRR_MSG_PULSE, 6);
  buf.writeUInt32LE(0, 8); // flags
  buf.writeBigUInt64LE(BigInt(0), 12);
  buf.writeUInt32LE(16, 20); // payload len = 16

  buf.writeUInt32LE(code, IPC_HEADER_SIZE);
  buf.writeUInt32LE(0, IPC_HEADER_SIZE + 4);
  buf.writeBigUInt64LE(BigInt(value), IPC_HEADER_SIZE + 8);

  return buf;
}

/**
 * Encodes an orderly disconnect frame.
 */
function encodeIpcGoodbye() {
  const buf = Buffer.alloc(IPC_HEADER_SIZE);
  buf.writeUInt32LE(SRR_WIRE_MAGIC, 0);
  buf.writeUInt16LE(SRR_WIRE_VERSION, 4);
  buf.writeUInt16LE(SRR_MSG_GOODBYE, 6);
  buf.writeUInt32LE(0, 8);
  buf.writeBigUInt64LE(BigInt(0), 12);
  buf.writeUInt32LE(0, 20);
  return buf;
}

/**
 * Decodes a 24-byte IPC frame header.
 */
function decodeIpcHeader(buf) {
  if (buf.length < IPC_HEADER_SIZE) {
    throw new Error(`Buffer too short for IPC header: ${buf.length} bytes`);
  }
  const magic = buf.readUInt32LE(0);
  const version = buf.readUInt16LE(4);
  const msgType = buf.readUInt16LE(6);
  const flags = buf.readUInt32LE(8);
  const transId = buf.readBigUInt64LE(12);
  const payloadLen = buf.readUInt32LE(20);

  if (magic !== SRR_WIRE_MAGIC) {
    throw new Error(`Invalid SRR magic: 0x${magic.toString(16)}`);
  }
  if (version !== SRR_WIRE_VERSION) {
    throw new Error(`Unsupported SRR version: ${version}`);
  }

  return { magic, version, msgType, flags, transId: Number(transId), payloadLen };
}

/**
 * Packs an SRR UDP datagram: [32B Header] + [Payload] + [2B CRC16-LE]
 */
function packUdpDatagram({ msgType, serviceId, operationId, sessionId, transId, payload }) {
  const payloadBuf = Buffer.isBuffer(payload) ? payload : Buffer.from(payload || '');
  const bodyLen = WIRE_HEADER_SIZE + payloadBuf.length;
  const dgram = Buffer.alloc(bodyLen + CRC_SIZE);

  dgram.writeUInt32LE(SRR_WIRE_MAGIC, 0);
  dgram.writeUInt16LE(SRR_WIRE_VERSION, 4);
  dgram.writeUInt16LE(msgType, 6);
  dgram.writeUInt16LE(0, 8); // flags
  dgram.writeUInt16LE(serviceId, 10);
  dgram.writeUInt16LE(operationId, 12);
  dgram.writeUInt16LE(0, 14); // reserved
  dgram.writeUInt32LE(sessionId, 16);
  dgram.writeBigUInt64LE(BigInt(transId), 20);
  dgram.writeUInt32LE(payloadBuf.length, 28);

  if (payloadBuf.length > 0) {
    payloadBuf.copy(dgram, WIRE_HEADER_SIZE);
  }

  const crc = srrCrc16(dgram.subarray(0, bodyLen));
  dgram.writeUInt16LE(crc, bodyLen);

  return dgram;
}

/**
 * Unpacks and verifies an SRR UDP datagram.
 */
function unpackUdpDatagram(dgram) {
  if (dgram.length < WIRE_HEADER_SIZE + CRC_SIZE) {
    throw new Error(`Datagram too short: ${dgram.length} bytes`);
  }

  const bodyLen = dgram.length - CRC_SIZE;
  const expectedCrc = dgram.readUInt16LE(bodyLen);
  const calcCrc = srrCrc16(dgram.subarray(0, bodyLen));

  if (expectedCrc !== calcCrc) {
    throw new Error(`CRC-16 mismatch: expected 0x${calcCrc.toString(16)}, got 0x${expectedCrc.toString(16)}`);
  }

  const magic = dgram.readUInt32LE(0);
  const version = dgram.readUInt16LE(4);
  const msgType = dgram.readUInt16LE(6);
  const flags = dgram.readUInt16LE(8);
  const serviceId = dgram.readUInt16LE(10);
  const operationId = dgram.readUInt16LE(12);
  const reserved = dgram.readUInt16LE(14);
  const sessionId = dgram.readUInt32LE(16);
  const transId = dgram.readBigUInt64LE(20);
  const payloadLen = dgram.readUInt32LE(28);

  if (magic !== SRR_WIRE_MAGIC || version !== SRR_WIRE_VERSION) {
    throw new Error('Invalid magic or protocol version in UDP datagram');
  }

  const payload = dgram.subarray(WIRE_HEADER_SIZE, WIRE_HEADER_SIZE + payloadLen);
  return {
    header: {
      magic,
      version,
      msgType,
      flags,
      serviceId,
      operationId,
      reserved,
      sessionId,
      transId: Number(transId),
      payloadLen
    },
    payload
  };
}

module.exports = {
  SRR_WIRE_MAGIC,
  SRR_WIRE_VERSION,
  SRR_MSG_HELLO,
  SRR_MSG_REQUEST,
  SRR_MSG_REPLY,
  SRR_MSG_ERROR,
  SRR_MSG_PULSE,
  SRR_MSG_CANCEL,
  SRR_MSG_GOODBYE,
  IPC_HEADER_SIZE,
  WIRE_HEADER_SIZE,
  CRC_SIZE,
  srrCrc16,
  encodeIpcRequest,
  encodeIpcPulse,
  encodeIpcGoodbye,
  decodeIpcHeader,
  packUdpDatagram,
  unpackUdpDatagram
};
