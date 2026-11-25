# SRR for Elixir

An idiomatic Elixir client and GenServer adapter for **SRR** (Synchronous Send / Receive / Reply message passing).

---

## Installation

Add `srr` to your dependencies in `mix.exs`:

```elixir
def deps do
  [
    {:srr, "~> 0.1.0"}
  ]
end
```

---

## Usage

### Supervised Worker in Application Supervision Tree
```elixir
children = [
  {SRR.Client, "srr_synth"}
]

Supervisor.start_link(children, strategy: :one_for_one)
```

### Direct Call (Synchronous SRR)
```elixir
# Start client
{:ok, client} = SRR.start_link("srr_synth")

# Synchronous request/reply
{:ok, reply} = SRR.call(client, "SET cutoff 1750.0\0")
IO.puts("Received: #{reply}") # "OK cutoff=1750.0 Hz"

# Asynchronous pulse (e.g. clock tick)
:ok = SRR.pulse(client, 1, 16)

# Disconnect
SRR.stop(client)
```

---

## Error Handling

Server error replies and transport errors are decoded directly into atoms:

| SRR C Status | Elixir Error |
|---|---|
| `SRR_TIMEOUT` | `{:error, :timeout}` |
| `SRR_CANCELLED` | `{:error, :cancelled}` |
| `SRR_DISCONNECTED` | `{:error, :disconnected}` |
| `SRR_SERVER_DIED` | `{:error, :server_died}` |
| `SRR_CLOSED` | `{:error, :closed}` |
| `SRR_QUEUE_FULL` | `{:error, :queue_full}` |
| `SRR_TOO_LARGE` | `{:error, :too_large}` |
