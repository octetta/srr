# Scripting, Automation & Multi-Language Integration Guide

SRR’s synchronous **Send / Receive / Reply** architecture is uniquely suited for scripting languages: it turns background system services, microcontrollers, and IPC daemons into synchronous, blocking function calls that fit directly into procedural scripts without complex event loops or callback spaghetti.

---

## Language Examples Overview (`examples/scripting/`)

The repository includes zero-dependency client implementations across 7 scripting environments:

| Language | File | Transport Mechanism | External Dependencies |
|---|---|---|:---:|
| **Bash / Shell** | [`shell_automation.sh`](../examples/scripting/shell_automation.sh) | `tools/srr_cli` | **None** (pure Bash) |
| **Python** | [`srr_client.py`](../examples/scripting/srr_client.py) | Unix Domain Socket (`socket`, `struct`) | **None** (Standard library) |
| **Perl** | [`srr_client.pl`](../examples/scripting/srr_client.pl) | Unix Domain Socket (`IO::Socket::UNIX`) | **None** (Core Perl) |
| **Tcl** | [`srr_client.tcl`](../examples/scripting/srr_client.tcl) | Native pipes / CLI | **None** (Core Tcl) |
| **PHP** | [`srr_client.php`](../examples/scripting/srr_client.php) | Unix Domain Socket (`stream_socket_client`) | **None** (Core PHP) |
| **PowerShell** | [`srr_client.ps1`](../examples/scripting/srr_client.ps1) | Windows Named Pipe / Unix Socket (.NET) | **None** (Standard .NET) |
| **Gforth** | [`srr_client.fs`](../examples/scripting/srr_client.fs) | Forth stack & subprocess bridge | **None** (Standard Forth) |

---

## 1. Bash / POSIX Shell

Using [`tools/srr_cli`](../tools/srr_cli.c), shell scripts can control devices and services synchronously:

```bash
# Query status into a variable
FIRMWARE=$(./tools/srr_cli --serial /dev/ttyACM0 info)

# Deterministic exit-code check
if ! ./tools/srr_cli --ipc audio_service ping > /dev/null 2>&1; then
    echo "Daemon unavailable!" >&2
    exit 2
fi

# Synchronous command execution
REPLY=$(./tools/srr_cli --ipc audio_service req 0 0 "SET cutoff 2400.0")
```

---

## 2. Python (Zero Third-Party Packages)

```python
from examples.scripting.srr_client import SRRClient

with SRRClient("srr_synth") as client:
    # Synchronously send and receive
    reply = client.send(b"STATUS")
    print("Received:", reply.decode())

    # Send asynchronous notification pulse
    client.pulse(code=1, value=42)
```

---

## 3. Perl (Pure Core)

```perl
use IO::Socket::UNIX;

# Open socket directly into /tmp/srr_synth.sock
my $sock = IO::Socket::UNIX->new(Peer => "/tmp/srr_synth.sock", Type => SOCK_STREAM);

# Send 24-byte header + payload
my $hdr = pack("V v v V Q< V", 0x53525231, 1, 2, 0, 1, length("STATUS"));
$sock->syswrite($hdr . "STATUS");

# Read reply header and payload
my $raw_hdr;
$sock->sysread($raw_hdr, 24);
my ($magic, $ver, $type, $flags, $tid, $len) = unpack("V v v V Q< V", $raw_hdr);
my $reply;
$sock->sysread($reply, $len);
print "Reply: $reply\n";
```

---

## 4. PHP

```php
$sock = stream_socket_client("unix:///tmp/srr_synth.sock");

// Pack header: uint32 magic, uint16 ver, uint16 type, uint32 flags, uint64 tid, uint32 len
$hdr = pack("VvvVPV", 0x53525231, 1, 2, 0, 1, strlen("STATUS"));
fwrite($sock, $hdr . "STATUS");

// Read response
$raw_hdr = fread($sock, 24);
$fields = unpack("Vmagic/vver/vtype/Vflags/Ptid/Vlen", $raw_hdr);
$reply = fread($sock, $fields['len']);
echo "Reply: " . $reply;
```

---

## 5. PowerShell (.NET Named Pipes & Sockets)

PowerShell scripts automatically switch between **Windows Named Pipes** (`\\.\pipe\srr_*`) on Windows and **Unix Domain Sockets** (`/tmp/srr_*.sock`) on Linux/macOS:

```powershell
# In PowerShell:
. ./examples/scripting/srr_client.ps1
$reply = Send-SRRRequest -Service "srr_synth" -Payload "STATUS"
Write-Host "Service replied: $reply"
```

---

## 6. Gforth

Gforth scripts can interact directly via stack-driven wrappers:

```forth
include examples/scripting/srr_client.fs
srr-ping
srr-query
```

---

## 7. Scripting Languages as SRR Servers

Beyond acting as clients, **scripting languages can act as full-fledged SRR services** that answer synchronous requests from C applications, Erlang nodes, WebAudio UIs, and microcontrollers.

There are two primary patterns for building scripting servers:
1. **The Piped Bridge (`srr_cli serve --exec <script>`)**: The "inetd / CGI" model.
2. **Native Socket Listeners**: Direct binary framing in languages with socket APIs (Python, Perl, PHP, Node.js).

---

### Pattern 1: The Piped Bridge (`srr_cli serve`)

In this architecture, `tools/srr_cli` manages all the heavy C mechanics: binding the IPC socket (or UDP port), enforcing single-winner timeouts, handling queue backpressure (`SRR_QUEUE_FULL`), and serializing the binary 24-byte headers. 

When a transaction arrives, `srr_cli` feeds the request payload into the script via `stdin` and captures the script's `stdout` as the synchronous `srr_reply()`:

```text
  [Client (C / Erlang / MCU)]
              |
              | Synchronous srr_send()
              v
  +-------------------------------------------------------+
  | tools/srr_cli --ipc sysmon serve --exec ./server.sh   |
  |  - Binds /tmp/srr_sysmon.sock                         |
  |  - Single-winner transaction state machine            |
  |  - Bounded queue & timeout protection                 |
  +-------------------------------------------------------+
              | stdin (payload)       ^ stdout (reply)
              v                       |
  +-------------------------------------------------------+
  | Script Handler (Bash, Python, Perl, Ruby, Node, etc.) |
  +-------------------------------------------------------+
```

#### Bash Example ([`examples/scripting/shell_server.sh`](../examples/scripting/shell_server.sh))
```bash
#!/usr/bin/env bash
read -r REQUEST
case "$REQUEST" in
    "PING")  echo "PONG from Bash SRR Server!" ;;
    "DISK")  df -h / | awk 'NR==2 {print $4}' ;;
    *)       echo "ERR_UNKNOWN: $REQUEST" ;;
esac
```
Run as service:
```bash
./tools/srr_cli --ipc sysmon serve --exec ./examples/scripting/shell_server.sh
```

#### Python Handler (`server.py`)
```python
#!/usr/bin/env python3
import sys, json

payload = sys.stdin.read().strip()
if payload == "STATS":
    response = {"status": "ok", "cores": 8, "memory_free_mb": 4096}
else:
    response = {"error": "unsupported_command"}

sys.stdout.write(json.dumps(response))
```

#### Perl Handler (`server.pl`)
```perl
#!/usr/bin/env perl
use strict;
my $req = <STDIN>;
chomp($req);
print "PERL_ECHO:$req";
```

#### PHP Handler (`server.php`)
```php
#!/usr/bin/env php
<?php
$req = trim(file_get_contents("php://stdin"));
echo "PHP_PROCESSED:" . strtoupper($req);
```

#### Node.js Handler (`server.js`)
```javascript
#!/usr/bin/env node
const fs = require('fs');
const req = fs.readFileSync(0, 'utf-8').trim();
process.stdout.write(`NODE_ACK:${req.length}_bytes`);
```

---

### Pattern 2: Native Script Sockets (Persistent In-Memory State)

If a scripting service needs persistent state across requests (e.g., an in-memory cache, active machine learning model, or long-lived database connection), the script can bind the Unix Domain Socket directly using the standard 24-byte binary IPC format.

#### Native Python SRR Server Example
```python
import socket, struct

SRR_MAGIC = 0x53525231
SRR_VERSION = 1
SRR_MSG_REQUEST = 2
SRR_MSG_REPLY = 3

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind("/tmp/srr_py_service.sock")
server.listen(16)

state_counter = 0

while True:
    conn, _ = server.accept()
    # Read 24-byte header
    hdr = conn.recv(24)
    magic, ver, msg_type, flags, tid, plen = struct.unpack("<IHHIQI", hdr)
    req_body = conn.recv(plen).decode('utf-8')

    state_counter += 1
    reply_body = f"COUNT={state_counter} RECEIVED={req_body}".encode('utf-8')

    # Send 24-byte reply header + payload
    rep_hdr = struct.pack("<IHHIQI", SRR_MAGIC, SRR_VERSION, SRR_MSG_REPLY, 0, tid, len(reply_body))
    conn.sendall(rep_hdr + reply_body)
    conn.close()
```

---

### Comparison of Scripting Server Approaches

| Feature | Piped Bridge (`srr_cli serve`) | Native Script Sockets |
|---|---|---|
| **Scripting Languages Supported** | **Any** (Bash, Python, Perl, Tcl, PHP, Ruby, Node, Awk, Jq) | Only languages with binary socket APIs |
| **Concurrency & Single-Winner** | Handled completely by C runtime | Script must handle threading & signals |
| **Crash Protection** | Script crash returns cleanly with exit code | Script crash kills the socket |
| **State Persistence** | Stateless per-request (or writes to disk/DB) | Stateful in-memory process |
| **Implementation Complexity** | **Zero socket code** in the script | Requires struct packing/unpacking loop |

