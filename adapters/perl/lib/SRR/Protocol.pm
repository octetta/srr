package SRR::Protocol;

use strict;
use warnings;
use Exporter 'import';

our @EXPORT_OK = qw(
    SRR_WIRE_MAGIC
    SRR_WIRE_VERSION
    SRR_MSG_HELLO
    SRR_MSG_REQUEST
    SRR_MSG_REPLY
    SRR_MSG_ERROR
    SRR_MSG_PULSE
    SRR_MSG_CANCEL
    SRR_MSG_GOODBYE
    IPC_HEADER_SIZE
    WIRE_HEADER_SIZE
    srr_crc16
    encode_ipc_request
    encode_ipc_pulse
    encode_ipc_goodbye
    decode_ipc_header
    pack_udp_datagram
    unpack_udp_datagram
);

our %EXPORT_TAGS = (all => \@EXPORT_OK);


use constant {
    SRR_WIRE_MAGIC   => 0x53525231, # "SRR1"
    SRR_WIRE_VERSION => 1,
    SRR_MSG_HELLO    => 1,
    SRR_MSG_REQUEST  => 2,
    SRR_MSG_REPLY    => 3,
    SRR_MSG_ERROR    => 4,
    SRR_MSG_PULSE    => 5,
    SRR_MSG_CANCEL   => 6,
    SRR_MSG_GOODBYE  => 7,
    IPC_HEADER_SIZE  => 24,
    WIRE_HEADER_SIZE => 32,
    CRC_SIZE         => 2,
};

my $IPC_FMT  = "V v v V Q< V";
my $WIRE_FMT = "V v v v v v v V Q< V";

sub srr_crc16 {
    my ($data, $init_crc) = @_;
    $init_crc //= 0xFFFF;
    my $crc = $init_crc & 0xFFFF;
    for my $b (unpack("C*", $data)) {
        $crc ^= ($b << 8) & 0xFFFF;
        for (1..8) {
            if ($crc & 0x8000) {
                $crc = (($crc << 1) ^ 0x1021) & 0xFFFF;
            } else {
                $crc = ($crc << 1) & 0xFFFF;
            }
        }
    }
    return $crc;
}

sub encode_ipc_request {
    my ($trans_id, $payload) = @_;
    $payload //= "";
    my $hdr = pack($IPC_FMT, SRR_WIRE_MAGIC, SRR_WIRE_VERSION, SRR_MSG_REQUEST, 0, $trans_id, length($payload));
    return $hdr . $payload;
}

sub encode_ipc_pulse {
    my ($code, $value) = @_;
    my $body = pack("V V Q<", $code, 0, $value);
    my $hdr = pack($IPC_FMT, SRR_WIRE_MAGIC, SRR_WIRE_VERSION, SRR_MSG_PULSE, 0, 0, length($body));
    return $hdr . $body;
}

sub encode_ipc_goodbye {
    return pack($IPC_FMT, SRR_WIRE_MAGIC, SRR_WIRE_VERSION, SRR_MSG_GOODBYE, 0, 0, 0);
}

sub decode_ipc_header {
    my ($raw_hdr) = @_;
    die "Truncated header\n" if length($raw_hdr) < IPC_HEADER_SIZE;
    my ($magic, $ver, $msg_type, $flags, $trans_id, $plen) = unpack($IPC_FMT, substr($raw_hdr, 0, IPC_HEADER_SIZE));
    die sprintf("Invalid magic: 0x%X\n", $magic) unless $magic == SRR_WIRE_MAGIC;
    die "Invalid version: $ver\n" unless $ver == SRR_WIRE_VERSION;
    return {
        magic       => $magic,
        version     => $ver,
        msg_type    => $msg_type,
        flags       => $flags,
        trans_id    => $trans_id,
        payload_len => $plen,
    };
}

sub pack_udp_datagram {
    my (%args) = @_;
    my $payload = $args{payload} // "";
    my $hdr = pack(
        $WIRE_FMT,
        SRR_WIRE_MAGIC,
        SRR_WIRE_VERSION,
        $args{msg_type} // SRR_MSG_REQUEST,
        0,
        $args{service_id} // 0,
        $args{operation_id} // 0,
        0,
        $args{session_id} // 0,
        $args{trans_id} // 0,
        length($payload)
    );
    my $body = $hdr . $payload;
    my $crc = srr_crc16($body);
    return $body . pack("v", $crc);
}

sub unpack_udp_datagram {
    my ($dgram) = @_;
    die "Datagram too short\n" if length($dgram) < WIRE_HEADER_SIZE + CRC_SIZE;
    my $body = substr($dgram, 0, length($dgram) - CRC_SIZE);
    my $expected_crc = unpack("v", substr($dgram, -CRC_SIZE));
    my $calc_crc = srr_crc16($body);
    die "CRC-16 mismatch\n" unless $expected_crc == $calc_crc;

    my ($magic, $ver, $msg_type, $flags, $s_id, $op_id, $res, $sess_id, $tid, $plen) =
        unpack($WIRE_FMT, substr($body, 0, WIRE_HEADER_SIZE));
    die "Invalid magic\n" unless $magic == SRR_WIRE_MAGIC;

    my $payload = substr($body, WIRE_HEADER_SIZE, $plen);
    return (
        {
            magic        => $magic,
            version      => $ver,
            msg_type     => $msg_type,
            service_id   => $s_id,
            operation_id => $op_id,
            session_id   => $sess_id,
            trans_id     => $tid,
            payload_len  => $plen,
        },
        $payload
    );
}

1;
