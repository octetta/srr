package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	srr "github.com/octetta/srr/adapters/go"
)

type MathService struct {
	requestCount uint64
	pulseCount   uint64
	startTime    time.Time
}

func (s *MathService) HandleRequest(req []byte) ([]byte, error) {
	atomic.AddUint64(&s.requestCount, 1)
	cmd := strings.TrimSpace(string(req))
	parts := strings.SplitN(cmd, " ", 2)
	action := strings.ToUpper(parts[0])

	switch action {
	case "PING":
		return []byte("PONG"), nil

	case "STATUS":
		uptime := time.Since(s.startTime).Truncate(time.Millisecond)
		reqs := atomic.LoadUint64(&s.requestCount)
		pulses := atomic.LoadUint64(&s.pulseCount)
		return []byte(fmt.Sprintf("STATUS service=go_math uptime=%s requests=%d pulses=%d", uptime, reqs, pulses)), nil

	case "ECHO":
		arg := ""
		if len(parts) > 1 {
			arg = parts[1]
		}
		return []byte("ECHO " + arg), nil

	case "DOUBLE":
		if len(parts) < 2 {
			return nil, fmt.Errorf("missing argument for DOUBLE")
		}
		var val int64
		if _, err := fmt.Sscanf(parts[1], "%d", &val); err != nil {
			return nil, fmt.Errorf("invalid integer: %s", parts[1])
		}
		return []byte(fmt.Sprintf("%d", val*2)), nil

	default:
		return nil, fmt.Errorf("unknown command: %s", action)
	}
}

func (s *MathService) HandlePulse(code uint32, val uint64) {
	atomic.AddUint64(&s.pulseCount, 1)
	fmt.Printf("[PULSE EVENT] code=%d value=%d (total pulses: %d)\n", code, val, atomic.LoadUint64(&s.pulseCount))
}

func main() {
	service := flag.String("service", "go_service", "Service name to expose")
	flag.Parse()

	svc := &MathService{
		startTime: time.Now(),
	}

	server := srr.NewServer(*service, svc)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-sigCh
		fmt.Println("\nReceived termination signal. Shutting down SRR Go server...")
		_ = server.Shutdown()
	}()

	fmt.Printf("--- SRR Go Microservice '%s' listening on /tmp/srr_%s.sock ---\n", *service, *service)
	fmt.Println("Available commands: PING, STATUS, ECHO <text>, DOUBLE <int>")

	if err := server.ListenAndServe(); err != nil {
		log.Fatalf("Server exited with error: %v\n", err)
	}
	fmt.Println("Server stopped cleanly.")
}
