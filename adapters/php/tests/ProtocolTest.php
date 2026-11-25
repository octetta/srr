<?php

require_once __DIR__ . '/../src/Protocol.php';

use SRR\Protocol;

function assert_test($cond, $msg) {
    if (!$cond) {
        echo "FAILED: $msg\n";
        exit(1);
    }
    echo "  [PASS] $msg\n";
}

echo "Running PHP SRR Adapter unit tests...\n";

// 1. CRC16
$crc = Protocol::crc16("123456789");
assert_test($crc === 0x29B1, "CRC-16 CCITT vector matches 0x29B1");

// 1b. COBS encoding & decoding
$raw = hex2bin("00010000020304000500");
$enc = bin2hex(Protocol::cobsEncode($raw));
assert_test($enc === "0102010104020304020501", "COBS encode vector matches");
$dec = Protocol::cobsDecode(hex2bin($enc));
assert_test($dec === $raw, "COBS decode roundtrip matches");

// 1c. Wire frame with COBS
$wf = bin2hex(Protocol::encodeWireFrame(Protocol::MSG_REQUEST, 42, 7, 1001, 999999, "hello_srr"));
assert_test($wf === "06315252530102020101022a0207010103e90301043f420f01010101020901010c68656c6c6f5f737272ce3e00", "Wire frame vector matches");

// 2. IPC Request framing
$req = Protocol::encodeIpcRequest(99, "PHP_DATA");
assert_test(strlen($req) === 24 + 8, "IPC request frame length is 32 bytes");
$hdr = Protocol::decodeIpcHeader(substr($req, 0, 24));
assert_test($hdr['trans_id'] === 99, "Transaction ID matches");
assert_test($hdr['payload_len'] === 8, "Payload len matches");

// 3. IPC Pulse framing
$pulse = Protocol::encodeIpcPulse(4, 2048);
$pHdr = Protocol::decodeIpcHeader(substr($pulse, 0, 24));
assert_test($pHdr['msg_type'] === Protocol::MSG_PULSE, "Pulse message type matches");

// 4. UDP Datagram pack/unpack
$dgram = Protocol::packUdpDatagram(Protocol::MSG_REQUEST, 1, 2, 333, 77, "HELLO_UDP");
$res = Protocol::unpackUdpDatagram($dgram);
assert_test($res['payload'] === "HELLO_UDP", "UDP roundtrip payload matches");

echo "All PHP tests passed successfully!\n";
