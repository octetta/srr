#!/usr/bin/env perl
# ==============================================================================
# SRR Perl Client (Zero CPAN Dependencies)
# Direct Unix Domain Socket communication using IO::Socket::UNIX and pack/unpack.
# ==============================================================================

use strict;
use warnings;
use IO::Socket::UNIX;
use Time::HiRes qw(gettimeofday tv_interval);

my $SRR_PROTO_MAGIC   = 0x53525231; # "SRR1"
my $SRR_PROTO_VERSION = 1;
my $SRR_MSG_REQUEST   = 2;
my $SRR_MSG_REPLY     = 3;
my $SRR_MSG_PULSE     = 5;
my $SRR_MSG_GOODBYE   = 7;

# Header format: V (32-bit LE), v (16-bit LE), v (16-bit LE), V (32-bit LE), Q< (64-bit LE), V (32-bit LE)
my $HEADER_FMT = "V v v V Q< V";
my $HEADER_LEN = 24;

sub srr_connect {
    my ($service_name) = @_;
    my $sock_path = "/tmp/srr_${service_name}.sock";
    my $sock = IO::Socket::UNIX->new(
        Peer => $sock_path,
        Type => SOCK_STREAM,
        Timeout => 2
    ) or die "Cannot connect to $sock_path: $!\n";
    return $sock;
}

sub srr_send {
    my ($sock, $payload, $trans_id) = @_;
    $trans_id //= 1;
    my $payload_len = length($payload);

    # 1. Send Request Frame
    my $hdr = pack($HEADER_FMT, $SRR_PROTO_MAGIC, $SRR_PROTO_VERSION, $SRR_MSG_REQUEST, 0, $trans_id, $payload_len);
    $sock->syswrite($hdr . $payload) or die "Write failed: $!\n";

    # 2. Read Reply Header
    my $raw_hdr = "";
    $sock->sysread($raw_hdr, $HEADER_LEN) == $HEADER_LEN or die "Read header failed: $!\n";
    my ($magic, $ver, $msg_type, $flags, $rep_tid, $rep_len) = unpack($HEADER_FMT, $raw_hdr);

    die "Invalid magic: $magic\n" unless $magic == $SRR_PROTO_MAGIC;

    # 3. Read Reply Payload
    my $reply = "";
    if ($rep_len > 0) {
        $sock->sysread($reply, $rep_len) == $rep_len or die "Read payload failed: $!\n";
    }
    return $reply;
}

sub srr_pulse {
    my ($sock, $code, $value) = @_;
    my $pulse_body = pack("V V Q<", $code, 0, $value);
    my $hdr = pack($HEADER_FMT, $SRR_PROTO_MAGIC, $SRR_PROTO_VERSION, $SRR_MSG_PULSE, 0, 0, length($pulse_body));
    $sock->syswrite($hdr . $pulse_body) or die "Pulse write failed: $!\n";
}

# --- Demonstration ---
my $svc = "srr_synth";
print "Connecting Perl client to '$svc'...\n";

eval {
    my $sock = srr_connect($svc);

    my $t0 = [gettimeofday];
    my $reply = srr_send($sock, "STATUS", 1);
    my $elapsed_us = tv_interval($t0) * 1_000_000;
    print sprintf("[REPLY in %.1f us]: %s\n", $elapsed_us, $reply);

    $reply = srr_send($sock, "SET cutoff 3200.0", 2);
    print "[REPLY]: $reply\n";

    srr_pulse($sock, 1, 8);
    print "[PULSE]: Clock tick emitted successfully.\n";

    # Goodbye and close
    my $bye_hdr = pack($HEADER_FMT, $SRR_PROTO_MAGIC, $SRR_PROTO_VERSION, $SRR_MSG_GOODBYE, 0, 0, 0);
    $sock->syswrite($bye_hdr);
    $sock->close();
};
if ($@) {
    warn "Perl client note: $@";
}
