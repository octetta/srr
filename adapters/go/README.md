# SRR Go Adapter

A high-performance, idiomatic Go client, server, and protocol implementation for **SRR (Synchronous Send/Receive/Reply)** with **zero external dependencies**.

Uses only Go's standard library (`net`, `encoding/binary`, `sync`, `time`).

---

## Features

- **Zero External Dependencies:** Built entirely with Go standard library.
- **Client & Server in Pure Go:** Write both SRR clients and native high-throughput SRR servers in Go.
- **Unix Domain Socket IPC:** Fast local IPC communicating directly with C, Rust, or other SRR nodes.
- **UDP Datagram Transport:** 32-byte binary protocol framing with hardware-compatible CRC-16-CCITT integrity checks.
- **One-Way Pulse Events:** Asynchronous notifications and clock pulses.

---

## Quick Start

### 1. Synchronous IPC Client

```go
package main

import (
    "fmt"
    "log"

    "github.com/octetta/srr/adapters/go"
)

func main() {
    // Connect to /tmp/srr_srr_synth.sock
    client, err := srr.Dial("srr_synth")
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // Send synchronous call
    reply, err := client.Call([]byte("STATUS"))
    if err != nil {
        log.Fatal(err)
    }
    fmt.Println("Synth reply:", string(reply))

    // Send asynchronous pulse
    _ = client.Pulse(1, 16)
}
```

### 2. High-Performance SRR Server in Go

```go
package main

import (
    "fmt"
    "log"

    "github.com/octetta/srr/adapters/go"
)

type MyService struct{}

func (s *MyService) HandleRequest(req []byte) ([]byte, error) {
    cmd := string(req)
    if cmd == "PING" {
        return []byte("PONG"), nil
    }
    return []byte("OK: " + cmd), nil
}

func (s *MyService) HandlePulse(code uint32, value uint64) {
    fmt.Printf("Received hardware pulse: code=%d, value=%d\n", code, value)
}

func main() {
    server := srr.NewServer("my_go_service", &MyService{})
    log.Println("Starting SRR Go server on /tmp/srr_my_go_service.sock...")
    if err := server.ListenAndServe(); err != nil {
        log.Fatal(err)
    }
}
```

### 3. UDP Client

```go
package main

import (
    "fmt"
    "time"

    "github.com/octetta/srr/adapters/go"
)

func main() {
    client, err := srr.DialUDP("127.0.0.1:8888")
    if err != nil {
        panic(err)
    }
    defer client.Close()

    reply, err := client.Call(1, 10, []byte("STATUS"), time.Second)
    if err != nil {
        panic(err)
    }
    fmt.Println("UDP reply:", string(reply))
}
```

---

## Running Tests

```bash
cd adapters/go && go test -v ./...
```
