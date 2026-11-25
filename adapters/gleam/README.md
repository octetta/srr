# SRR for Gleam (BEAM)

Type-safe Gleam client and wire framing for **SRR** (Synchronous Send / Receive / Reply message passing) on the Erlang BEAM runtime.

---

## Overview

Gleam integrates seamlessly with SRR on the BEAM runtime through two mechanisms:

1. **Direct Erlang OTP FFI (`srr_client.erl`):**
   Connects Gleam applications directly to the battle-tested Erlang `gen_server` client.
2. **Type-Safe BitArray Framing:**
   Construct and pattern-match SRR 24-byte wire frames directly using Gleam's bitstring pattern matching.

---

## Usage Example

```gleam
import srr

pub fn main() {
  // 1. Connect to local SRR synth service via Erlang FFI
  let assert Ok(client) = srr.connect("srr_synth")

  // 2. Synchronous Send / Receive / Reply transaction
  let request = <<"STATUS":utf8, 0:8>>
  let assert Ok(reply) = srr.call(client, request)

  // 3. Fire-and-forget one-way pulse (code=1, val=16)
  let _ = srr.pulse(client, 1, 16)

  // 4. Disconnect
  srr.stop(client)
}
```

### Direct BitArray Construction

```gleam
import srr

pub fn make_request() {
  let frame = srr.encode_request(1, <<"SET cutoff 1200\0":utf8>>)
  // Send over gen_tcp Unix Domain Socket...
}
```
