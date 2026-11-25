# SRR BEAM (Erlang / Elixir / Gleam) Adapter

This adapter provides two distinct integration pathways for BEAM applications to communicate with native C SRR services:

---

## 1. Direct Unix Domain Socket `gen_server` (`srr_client.erl`)

Uses Erlang's native Unix Domain Socket support (`gen_tcp:connect({local, Path}, ...)`):

```erlang
%% Start the client GenServer connected to "srr_synth"
{ok, Client} = srr_client:start_link("srr_synth").

%% Synchronous request/reply (maps to srr_send_timed)
{ok, Reply} = srr_client:call(Client, <<"STATUS">>).

%% Asynchronous pulse (maps to srr_pulse)
ok = srr_client:pulse(Client, 1, 16).

%% Stop
srr_client:stop(Client).
```

### In Elixir:
```elixir
# Direct binary pattern matching over UDS:
{:ok, socket} = :gen_tcp.connect({:local, ~c"/tmp/srr_srr_synth.sock"}, 0, [:binary, active: true, packet: 0])

# Synchronous request frame:
magic = 0x53525231
version = 1
msg_type_request = 2
tid = 1
payload = "SET cutoff 1800\0"
frame = <<magic::32-little, version::16-little, msg_type_request::16-little, 0::32, tid::64-little, byte_size(payload)::32-little, payload::binary>>
:gen_tcp.send(socket, frame)
```

---

## 2. Standard BEAM Port Driver (`srr_port_driver`)

For environments preferring supervised external OS processes communicating over stdio with 4-byte length prefixes (`{packet, 4}`):

### Erlang invocation:
```erlang
Port = open_port({spawn_executable, "adapters/beam/srr_port_driver"}, [{packet, 4}, binary]).

%% Connect to service
port_command(Port, <<1, "srr_synth">>).

%% Synchronous send
port_command(Port, <<2, "SET cutoff 2000\0">>).

%% Receive reply: <<0, "OK cutoff=2000.0">>
```

---

## Verification

Run the verification test suite:
```bash
python3 adapters/beam/test/test_beam_adapter.py
```
