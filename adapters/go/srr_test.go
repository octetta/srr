package srr

import (
	"bytes"
	"fmt"
	"os"
	"sync"
	"testing"
	"time"
)

func TestCRC16(t *testing.T) {
	// Standard CCITT-False vector "123456789" -> 0x29B1
	got := CRC16([]byte("123456789"), 0xFFFF)
	if got != 0x29B1 {
		t.Fatalf("CRC16 vector mismatch: expected 0x29B1, got 0x%04X", got)
	}
}

func TestIPCFraming(t *testing.T) {
	req := EncodeIPCRequest(42, []byte("HELLO_GO"))
	if len(req) != IPCHeaderSize+8 {
		t.Fatalf("Expected len %d, got %d", IPCHeaderSize+8, len(req))
	}

	hdr, err := DecodeIPCHeader(req[:IPCHeaderSize])
	if err != nil {
		t.Fatalf("DecodeIPCHeader error: %v", err)
	}

	if hdr.Magic != WireMagic {
		t.Errorf("Bad magic: 0x%X", hdr.Magic)
	}
	if hdr.Version != WireVersion {
		t.Errorf("Bad version: %d", hdr.Version)
	}
	if hdr.MsgType != MsgRequest {
		t.Errorf("Expected MsgRequest, got %d", hdr.MsgType)
	}
	if hdr.TransID != 42 {
		t.Errorf("Expected TransID 42, got %d", hdr.TransID)
	}
	if hdr.PayloadLen != 8 {
		t.Errorf("Expected PayloadLen 8, got %d", hdr.PayloadLen)
	}
	if string(req[IPCHeaderSize:]) != "HELLO_GO" {
		t.Errorf("Payload mismatch: %s", string(req[IPCHeaderSize:]))
	}
}

func TestIPCPulseFraming(t *testing.T) {
	pulse := EncodeIPCPulse(5, 1234567890)
	if len(pulse) != IPCHeaderSize+16 {
		t.Fatalf("Expected len %d, got %d", IPCHeaderSize+16, len(pulse))
	}

	hdr, err := DecodeIPCHeader(pulse[:IPCHeaderSize])
	if err != nil {
		t.Fatalf("DecodeIPCHeader error: %v", err)
	}
	if hdr.MsgType != MsgPulse {
		t.Errorf("Expected MsgPulse, got %d", hdr.MsgType)
	}
}

func TestUDPDatagram(t *testing.T) {
	payload := []byte("UDP_TEST_PAYLOAD")
	dgram := PackUDPDatagram(MsgRequest, 10, 20, 999, 101, payload)

	hdr, unpackedPayload, err := UnpackUDPDatagram(dgram)
	if err != nil {
		t.Fatalf("UnpackUDPDatagram error: %v", err)
	}

	if hdr.ServiceID != 10 {
		t.Errorf("ServiceID mismatch: %d", hdr.ServiceID)
	}
	if hdr.OperationID != 20 {
		t.Errorf("OperationID mismatch: %d", hdr.OperationID)
	}
	if hdr.SessionID != 999 {
		t.Errorf("SessionID mismatch: %d", hdr.SessionID)
	}
	if hdr.TransID != 101 {
		t.Errorf("TransID mismatch: %d", hdr.TransID)
	}
	if !bytes.Equal(unpackedPayload, payload) {
		t.Errorf("Payload mismatch: %s", string(unpackedPayload))
	}

	// Corrupt datagram and check CRC failure
	corrupted := make([]byte, len(dgram))
	copy(corrupted, dgram)
	corrupted[WireHeaderSize] ^= 0xFF

	if _, _, err := UnpackUDPDatagram(corrupted); err != ErrCRCMismatch {
		t.Fatalf("Expected ErrCRCMismatch, got %v", err)
	}
}

type testHandler struct {
	pulseReceived sync.WaitGroup
	lastPulseCode uint32
	lastPulseVal  uint64
	mu            sync.Mutex
}

func (h *testHandler) HandleRequest(req []byte) ([]byte, error) {
	s := string(req)
	if s == "PING" {
		return []byte("PONG"), nil
	}
	if s == "FAIL" {
		return nil, fmt.Errorf("intentional error")
	}
	return []byte("ECHO: " + s), nil
}

func (h *testHandler) HandlePulse(code uint32, val uint64) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.lastPulseCode = code
	h.lastPulseVal = val
	h.pulseReceived.Done()
}

func TestLoopbackClientServer(t *testing.T) {
	sockName := "go_loopback_test"
	sockPath := fmt.Sprintf("/tmp/srr_%s.sock", sockName)
	_ = os.Remove(sockPath)

	h := &testHandler{}
	h.pulseReceived.Add(1)

	server := NewServer(sockName, h)

	errCh := make(chan error, 1)
	go func() {
		errCh <- server.ListenAndServe()
	}()

	// Wait for server to bind
	time.Sleep(50 * time.Millisecond)

	client, err := Dial(sockName)
	if err != nil {
		t.Fatalf("Failed to dial: %v", err)
	}

	// 1. Successful request
	rep, err := client.Call([]byte("PING"))
	if err != nil {
		client.Close()
		t.Fatalf("Call PING failed: %v", err)
	}
	if string(rep) != "PONG" {
		client.Close()
		t.Fatalf("Expected PONG, got %s", string(rep))
	}

	// 2. Error handling
	_, err = client.Call([]byte("FAIL"))
	if err == nil {
		client.Close()
		t.Fatalf("Expected error, got nil")
	}

	// 3. Pulse handling
	if err := client.Pulse(7, 4096); err != nil {
		client.Close()
		t.Fatalf("Pulse failed: %v", err)
	}
	h.pulseReceived.Wait()

	if h.lastPulseCode != 7 || h.lastPulseVal != 4096 {
		client.Close()
		t.Fatalf("Pulse values mismatch: %d, %d", h.lastPulseCode, h.lastPulseVal)
	}

	_ = client.Close()

	// Shutdown
	if err := server.Shutdown(); err != nil {
		t.Fatalf("Server shutdown failed: %v", err)
	}
}
