#!/usr/bin/env perl
use strict;
use warnings;
use FindBin qw($Bin);
use lib "$Bin/../lib";
use Test::More tests => 9;

use SRR::Protocol qw(:all);

# 1. CRC16 test vector "123456789" -> 0x29B1
my $crc = srr_crc16("123456789");
is($crc, 0x29B1, "CRC-16 CCITT vector matches 0x29B1");

# 1b. COBS encoding vector
my $raw = "\x00\x01\x00\x00\x02\x03\x04\x00\x05\x00";
my $enc = unpack("H*", cobs_encode($raw));
is($enc, "0102010104020304020501", "COBS encode vector matches");

# 1c. Wire frame vector
my $wf = unpack("H*", encode_wire_frame(
    msg_type => 2,
    service_id => 42,
    operation_id => 7,
    session_id => 1001,
    trans_id => 999999,
    payload => "hello_srr"
));
is($wf, "06315252530102020101022a0207010103e90301043f420f01010101020901010c68656c6c6f5f737272ce3e00", "Wire frame vector matches");

# 2. IPC Request framing
my $req = encode_ipc_request(55, "PERL_PAYLOAD");
is(length($req), 24 + 12, "IPC request frame length is 36 bytes");
my $hdr = decode_ipc_header(substr($req, 0, 24));
is($hdr->{magic}, SRR_WIRE_MAGIC, "Magic matches SRR1");
is($hdr->{trans_id}, 55, "Transaction ID matches");
is($hdr->{payload_len}, 12, "Payload length matches");

# 3. IPC Pulse framing
my $pulse = encode_ipc_pulse(9, 1024);
my $pHdr = decode_ipc_header(substr($pulse, 0, 24));
is($pHdr->{msg_type}, SRR_MSG_PULSE, "Pulse msg_type matches");

# 4. UDP Datagram pack/unpack
my $dgram = pack_udp_datagram(
    service_id   => 3,
    operation_id => 7,
    session_id   => 888,
    trans_id     => 12,
    payload      => "UDP_PERL"
);
my ($uHdr, $uPayload) = unpack_udp_datagram($dgram);
is($uPayload, "UDP_PERL", "UDP unpack roundtrip payload matches");
