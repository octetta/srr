# SRR Ruby Adapter

A pure Ruby client and wire protocol library for **SRR (Synchronous Send/Receive/Reply)** with **zero gem dependencies**.

---

## Features

- **Zero Gem Dependencies:** Pure standard library (`socket`, `timeout`).
- **Synchronous IPC Client:** Connects to native C SRR services over Unix Domain Sockets (`/tmp/srr_<service>.sock`).
- **UDP Datagram Client:** 32-byte binary protocol framing with CRC-16-CCITT integrity validation.
- **One-Way Pulses:** Fast, non-blocking hardware notifications and clock ticks.

---

## Quick Example

```ruby
require_relative "lib/srr"

# 1. Connect to an SRR service
client = SRR::Client.new("srr_synth")
client.connect

# 2. Synchronous call
reply = client.call("STATUS")
puts "Reply: #{reply}"

# 3. Asynchronous pulse
client.pulse(1, 16)

client.close
```

---

## Running Tests

```bash
ruby adapters/ruby/test/test_client.rb
```
