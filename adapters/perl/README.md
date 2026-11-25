# SRR Perl Adapter

A pure Perl implementation of the **SRR (Synchronous Send/Receive/Reply)** protocol and client library with **zero CPAN dependencies**.

---

## Features

- **Zero CPAN Dependencies:** Runs out-of-the-box using core standard modules (`IO::Socket::UNIX`, `pack`/`unpack`).
- **Synchronous IPC Client:** Connects to native C SRR services over Unix Domain Sockets (`/tmp/srr_<service>.sock`).
- **UDP Datagram Support:** Architecture-independent 32-byte header with CRC-16 integrity verification.
- **Pulse Notifications:** Fast one-way events.

---

## Quick Example

```perl
use strict;
use warnings;
use SRR::Client;

my $client = SRR::Client->new("srr_synth");
$client->connect();

my $reply = $client->call("STATUS");
print "Received: $reply\n";

$client->pulse(1, 16);
$client->close();
```

---

## Running Tests

```bash
perl adapters/perl/t/01_protocol.t
```
