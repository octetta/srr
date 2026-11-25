# Go SRR Examples

This directory contains standalone Go examples demonstrating both an **SRR Client** and an **SRR Microservice Server** in pure Go with zero C dependencies.

---

## 1. Running the Go Client

The Go client connects to an existing SRR service (such as `synth_server` or the Go server below) over Unix Domain Sockets:

```bash
# Query the C synth server
go run ./examples/go/client --service srr_synth --cmd "STATUS"

# Send a parameter adjustment
go run ./examples/go/client --service srr_synth --cmd "SET cutoff 2500.0"

# Emit a hardware pulse
go run ./examples/go/client --service srr_synth --pulse-code 1 --pulse-val 16
```

---

## 2. Running the Pure Go SRR Microservice

The Go server exposes a native SRR endpoint on `/tmp/srr_go_service.sock` capable of serving requests from C, Python, Perl, Node, Ruby, PHP, or shell scripts:

```bash
# Start the Go server in one terminal
go run ./examples/go/server --service go_service

# Query it from the Go client
go run ./examples/go/client --service go_service --cmd "STATUS"
go run ./examples/go/client --service go_service --cmd "DOUBLE 21"

# Or query it directly from the srr CLI tool!
./tools/srr_cli --ipc go_service send "STATUS"
./tools/srr_cli --ipc go_service send "DOUBLE 1024"
```
