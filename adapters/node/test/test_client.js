'use strict';

const assert = require('assert');
const {
  srrCrc16,
  cobsEncode,
  cobsDecode,
  encodeWireFrame,
  decodeWireFrame,
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

  // Test 1b: COBS roundtrip
  const rawMixed = Buffer.from('00010000020304000500', 'hex');
  const enc = cobsEncode(rawMixed);
  assert.strictEqual(enc.toString('hex'), '0102010104020304020501');
  const dec = cobsDecode(enc);
  assert.deepStrictEqual(dec, rawMixed);
  console.log('  [PASS] COBS encoding/decoding vector passed');

  // Test 1c: Wire frame roundtrip
  const wf = encodeWireFrame({
    msgType: SRR_MSG_REQUEST,
    serviceId: 42,
    operationId: 7,
    sessionId: 1001,
    transId: 999999,
    payload: 'hello_srr'
  });
  assert.strictEqual(wf.toString('hex'), '06315252530102020101022a0207010103e90301043f420f01010101020901010c68656c6c6f5f737272ce3e00');
  const parsedWf = decodeWireFrame(wf);
  assert.strictEqual(parsedWf.header.serviceId, 42);
  assert.strictEqual(parsedWf.payload.toString(), 'hello_srr');
  console.log('  [PASS] Wire frame COBS vector passed');

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
