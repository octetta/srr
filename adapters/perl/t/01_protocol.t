#!/usr/bin/env perl
use strict;
use warnings;
use FindBin qw($Bin);
use lib "$Bin/../lib";
use Test::More tests => 7;

use SRR::Protocol qw(:all);

# 1. CRC16 test vector "123456789" -> 0x29B1
my $crc = srr_crc16("123456789");
is($crc, 0x29B1, "CRC-16 CCITT vector matches 0x29B1");

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
