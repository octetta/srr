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
