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
