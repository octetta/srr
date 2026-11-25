# SRR Node.js Adapter

A pure JavaScript / Node.js client and protocol implementation for **SRR (Synchronous Send/Receive/Reply)**.

Built exclusively on Node.js standard modules (`net`, `dgram`, `Buffer`) with **zero npm dependencies**.

---

## Features

- **Zero NPM Dependencies:** Runs directly on Node.js without `npm install` requirements.
- **Promise-Based API:** Fully asynchronous `async/await` client supporting high-throughput pipelining.
- **Unix Domain Socket IPC:** Fast local inter-process communication with native C SRR services.
- **UDP Datagram Support:** 32-byte binary header with CRC-16-CCITT integrity verification.
- **Pulse Notifications:** Non-blocking one-way fire-and-forget events.

---

## Usage

### 1. Synchronous IPC Request / Reply

```javascript
const { SRRClient } = require('./adapters/node');

async function main() {
  const client = new SRRClient('srr_synth');
  await client.connect();

  // Send request and await reply
  const reply = await client.call('STATUS');
  console.log('Status reply:', reply.toString('utf-8'));

  // Update parameter
  await client.call('SET cutoff 2400.0');

  // Emit asynchronous pulse
  await client.pulse(1, 16);

  client.close();
}

main().catch(console.error);
```

### 2. UDP Client

```javascript
const { SRRUDPClient } = require('./adapters/node');

async function runUdp() {
  const udpClient = new SRRUDPClient('127.0.0.1', 8888);

  const reply = await udpClient.call(1, 10, 'PING');
  console.log('UDP reply:', reply.toString());

  udpClient.close();
}
```

---

## Testing

```bash
node adapters/node/test/test_client.js
```
