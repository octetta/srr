package SRR::Client;

use strict;
use warnings;
use IO::Socket::UNIX;
use IO::Socket::INET;
use Time::HiRes qw(time);
use SRR::Protocol qw(:all);

sub new {
    my ($class, $service_name, %opts) = @_;
    my $socket_dir = $opts{socket_dir} // "/tmp";
    my $socket_path = ($service_name =~ m{^/})
        ? $service_name
        : "$socket_dir/srr_$service_name.sock";

    my $self = {
        service_name  => $service_name,
        socket_path   => $socket_path,
        sock          => undef,
        next_trans_id => 1,
    };
    return bless $self, $class;
}

sub connect {
    my ($self, $timeout) = @_;
    $timeout //= 2;
    return $self if $self->{sock};

    my $sock = IO::Socket::UNIX->new(
        Peer    => $self->{socket_path},
        Type    => SOCK_STREAM,
        Timeout => $timeout,
    ) or die "Cannot connect to $self->{socket_path}: $!\n";

    $self->{sock} = $sock;
    return $self;
}

sub close {
    my ($self) = @_;
    if ($self->{sock}) {
        eval {
            $self->{sock}->syswrite(encode_ipc_goodbye());
            $self->{sock}->close();
        };
        $self->{sock} = undef;
    }
}

sub call {
    my ($self, $payload, $timeout) = @_;
    $self->connect($timeout) unless $self->{sock};

    my $tid = $self->{next_trans_id}++;
    my $frame = encode_ipc_request($tid, $payload);
    $self->{sock}->syswrite($frame) or die "Failed to write request: $!\n";

    my $raw_hdr = "";
    my $n = $self->{sock}->sysread($raw_hdr, IPC_HEADER_SIZE);
    die "Incomplete header read\n" unless defined $n && $n == IPC_HEADER_SIZE;

    my $hdr = decode_ipc_header($raw_hdr);
    my $reply = "";
    if ($hdr->{payload_len} > 0) {
        my $rem = $hdr->{payload_len};
        while ($rem > 0) {
            my $buf = "";
            my $read_bytes = $self->{sock}->sysread($buf, $rem);
            die "Connection closed unexpectedly\n" unless $read_bytes && $read_bytes > 0;
            $reply .= $buf;
            $rem -= $read_bytes;
        }
    }

    if ($hdr->{msg_type} == SRR_MSG_ERROR) {
        die "Service returned error: $reply\n";
    }

    return $reply;
}

sub pulse {
    my ($self, $code, $value) = @_;
    $self->connect() unless $self->{sock};
    my $frame = encode_ipc_pulse($code, $value);
    $self->{sock}->syswrite($frame) or die "Pulse write failed: $!\n";
}

1;
