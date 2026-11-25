package main

import (
	"flag"
	"fmt"
	"log"
	"time"

	srr "github.com/octetta/srr/adapters/go"
)

func main() {
	service := flag.String("service", "srr_synth", "SRR service name or Unix socket path")
	cmd := flag.String("cmd", "STATUS", "Command to send to the service")
	pulseCode := flag.Uint("pulse-code", 1, "Pulse code to emit")
	pulseVal := flag.Uint64("pulse-val", 16, "Pulse value to emit")
	flag.Parse()

	fmt.Printf("--- Connecting Go SRR Client to service '%s' ---\n", *service)
	client, err := srr.Dial(*service)
	if err != nil {
		log.Fatalf("Failed to connect: %v\n", err)
	}
	defer client.Close()

	// 1. Send synchronous request
	t0 := time.Now()
	reply, err := client.CallTimeout([]byte(*cmd), 2*time.Second)
	if err != nil {
		log.Fatalf("Call failed: %v\n", err)
	}
	elapsed := time.Since(t0)
	fmt.Printf("[REPLY in %v]: %s\n", elapsed, string(reply))

	// 2. Emit asynchronous pulse notification
	if err := client.Pulse(uint32(*pulseCode), *pulseVal); err != nil {
		log.Fatalf("Pulse failed: %v\n", err)
	}
	fmt.Printf("[PULSE]: Emitted pulse code=%d val=%d successfully.\n", *pulseCode, *pulseVal)
}
