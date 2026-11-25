# SRR PHP Adapter

A pure PHP client and wire protocol library for **SRR (Synchronous Send/Receive/Reply)** with **zero PECL or Composer dependencies**.

---

## Features

- **Zero External Dependencies:** Built strictly on PHP core functions (`stream_socket_client`, `pack`/`unpack`).
- **Synchronous IPC Client:** Connects to native C SRR services over Unix Domain Sockets (`unix:///tmp/srr_<service>.sock`).
- **UDP Datagram Support:** Full binary frame packing/unpacking with CRC-16-CCITT integrity checks.
- **Pulse Notifications:** Fast fire-and-forget one-way signaling.

---

## Quick Example

```php
<?php
require_once __DIR__ . '/src/Protocol.php';
require_once __DIR__ . '/src/Client.php';

use SRR\Client;

$client = new Client('srr_synth');
$client->connect();

// Send synchronous request
$reply = $client->call('STATUS');
echo "Status: $reply\n";

// Emit one-way pulse
$client->pulse(1, 16);

$client->close();
```

---

## Running Tests

```bash
php adapters/php/tests/ProtocolTest.php
```
