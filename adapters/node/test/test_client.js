'use strict';

const assert = require('assert');
const {
  srrCrc16,
  encodeIpcRequest,
  encodeIpcPulse,
  decodeIpcHeader,
  packUdpDatagram,
  unpackUdpDatagram,
  SRR_WIRE_MAGIC,
  SRR_WIRE_VERSION,
  SRR_MSG_REQUEST,
  SRR_MSG_PULSE
} = require('../lib/protocol');

function runTests() {
  console.log('Running Node.js SRR Adapter unit tests...');

  // Test 1: CRC16 test vector "123456789" -> 0x29B1
  const crc = srrCrc16(Buffer.from('123456789'));
  assert.strictEqual(crc, 0x29B1, `Expected 0x29B1, got 0x${crc.toString(16)}`);
  console.log('  [PASS] CRC-16 CCITT test vector passed');

  // Test 2: IPC request framing
  const req = encodeIpcRequest(42, 'HELLO_NODE');
  assert.strictEqual(req.length, 24 + 10);
  const hdr = decodeIpcHeader(req);
  assert.strictEqual(hdr.magic, SRR_WIRE_MAGIC);
  assert.strictEqual(hdr.version, SRR_WIRE_VERSION);
  assert.strictEqual(hdr.msgType, SRR_MSG_REQUEST);
  assert.strictEqual(hdr.transId, 42);
  assert.strictEqual(hdr.payloadLen, 10);
  assert.strictEqual(req.subarray(24).toString(), 'HELLO_NODE');
  console.log('  [PASS] IPC Request framing passed');

  // Test 3: IPC pulse framing
  const pulse = encodeIpcPulse(7, 987654321);
  assert.strictEqual(pulse.length, 24 + 16);
  const pulseHdr = decodeIpcHeader(pulse);
  assert.strictEqual(pulseHdr.msgType, SRR_MSG_PULSE);
  assert.strictEqual(pulseHdr.payloadLen, 16);
  console.log('  [PASS] IPC Pulse framing passed');

  // Test 4: UDP datagram pack/unpack
  const dgram = packUdpDatagram({
    msgType: SRR_MSG_REQUEST,
    serviceId: 2,
    operationId: 5,
    sessionId: 9999,
    transId: 101,
    payload: Buffer.from('TEST_PAYLOAD')
  });
  assert.strictEqual(dgram.length, 32 + 12 + 2);

  const { header, payload } = unpackUdpDatagram(dgram);
  assert.strictEqual(header.serviceId, 2);
  assert.strictEqual(header.operationId, 5);
  assert.strictEqual(header.sessionId, 9999);
  assert.strictEqual(header.transId, 101);
  assert.strictEqual(payload.toString(), 'TEST_PAYLOAD');
  console.log('  [PASS] UDP datagram roundtrip passed');

  // Test 5: UDP corruption rejection
  const corrupted = Buffer.from(dgram);
  corrupted[33] ^= 0xAA;
  assert.throws(() => {
    unpackUdpDatagram(corrupted);
  }, /CRC-16 mismatch/);
  console.log('  [PASS] Corrupted UDP datagram rejected');

  console.log('All Node.js unit tests passed successfully!');
}

runTests();
