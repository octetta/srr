# SRR Python Adapter

A clean, standalone, zero-dependency Python implementation of the **SRR (Synchronous Send/Receive/Reply)** protocol and client library.

Built strictly on Python's standard library (`socket` and `struct`) with no external package requirements or C compiler dependencies.

---

## Features

- **Standard Library Only:** Zero third-party dependencies (`pip install` without compilation).
- **Unix Domain Socket IPC:** Direct high-throughput IPC client communicating with native C SRR services.
- **UDP Transport Support:** Native 32-byte header encoding, CRC-16-CCITT integrity verification, and datagram transaction matching.
- **Resolver Integration:** Query the SRR Resolver daemon to dynamically discover service endpoints.
- **Context Manager Support:** Pythonic `with SRRClient(...) as client:` lifecycle management.

---

## Quick Start

### 1. Synchronous IPC Request / Reply

```python
from srr import SRRClient

# Connects to /tmp/srr_srr_synth.sock
with SRRClient("srr_synth") as client:
    # Send request and block for reply
    reply = client.call("STATUS")
    print("Received reply:", reply.decode("utf-8"))

    # Send parameter update
    client.call("SET cutoff 1850.0")

    # Emit asynchronous pulse notification
    client.pulse(code=1, value=8)
```

### 2. UDP Network Client

```python
from srr import SRRUDPClient

# Connect to an SRR microservice listening on UDP port 8888
client = SRRUDPClient(host="127.0.0.1", port=8888)

# Send request (service_id=1, operation_id=10)
reply = client.call(service_id=1, operation_id=10, payload=b"PING")
print("UDP reply:", reply)

client.close()
```

### 3. Dynamic Service Discovery via Resolver

```python
from srr import SRRResolver, SRRClient

# Resolve endpoint for "srr_synth"
endpoint = SRRResolver.resolve_ipc("srr_synth")
if endpoint:
    with SRRClient(endpoint) as client:
        print("Reply:", client.call("STATUS"))
```

---

## Running Tests

```bash
python3 adapters/python/tests/test_client.py
```
