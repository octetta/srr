package srr

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Client is a synchronous Send/Receive/Reply IPC client connecting over Unix Domain Sockets.
type Client struct {
	socketPath  string
	conn        net.Conn
	mu          sync.Mutex
	nextTransID uint64
}

// Dial connects to an SRR service via Unix Domain Socket.
// Accepts either a short service name ("srr_synth") or an absolute socket path ("/tmp/srr_synth.sock").
func Dial(serviceName string) (*Client, error) {
	socketPath := serviceName
	if !strings.HasPrefix(serviceName, "/") {
		socketPath = fmt.Sprintf("/tmp/srr_%s.sock", serviceName)
	}

	conn, err := net.DialTimeout("unix", socketPath, 2*time.Second)
	if err != nil {
		return nil, fmt.Errorf("srr: failed to connect to %s: %w", socketPath, err)
	}

	return &Client{
		socketPath:  socketPath,
		conn:        conn,
		nextTransID: 1,
	}, nil
}

// Call sends a synchronous request payload and blocks until a reply is received or timeout expires.
func (c *Client) Call(payload []byte) ([]byte, error) {
	return c.CallTimeout(payload, 2*time.Second)
}

// CallTimeout performs a synchronous call with an explicit timeout duration.
func (c *Client) CallTimeout(payload []byte, timeout time.Duration) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn == nil {
		return nil, errors.New("srr: client is closed")
	}

	tid := atomic.AddUint64(&c.nextTransID, 1) - 1

	if timeout > 0 {
		_ = c.conn.SetDeadline(time.Now().Add(timeout))
	} else {
		_ = c.conn.SetDeadline(time.Time{})
	}

	reqFrame := EncodeIPCRequest(tid, payload)
	if _, err := c.conn.Write(reqFrame); err != nil {
		return nil, fmt.Errorf("srr: failed to send request: %w", err)
	}

	hdrBuf := make([]byte, IPCHeaderSize)
	if _, err := io.ReadFull(c.conn, hdrBuf); err != nil {
		return nil, fmt.Errorf("srr: failed to read reply header: %w", err)
	}

	hdr, err := DecodeIPCHeader(hdrBuf)
	if err != nil {
		return nil, err
	}

	var replyPayload []byte
	if hdr.PayloadLen > 0 {
		replyPayload = make([]byte, hdr.PayloadLen)
		if _, err := io.ReadFull(c.conn, replyPayload); err != nil {
			return nil, fmt.Errorf("srr: failed to read reply payload: %w", err)
		}
	}

	if hdr.MsgType == MsgError {
		return nil, fmt.Errorf("%w: %s", ErrRemoteService, string(replyPayload))
	}

	return replyPayload, nil
}

// Pulse sends an asynchronous one-way notification without waiting for a reply.
func (c *Client) Pulse(code uint32, value uint64) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn == nil {
		return errors.New("srr: client is closed")
	}

	pulseFrame := EncodeIPCPulse(code, value)
	_, err := c.conn.Write(pulseFrame)
	return err
}

// Close gracefully disconnects with a goodbye frame.
func (c *Client) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil {
		_, _ = c.conn.Write(EncodeIPCGoodbye())
		err := c.conn.Close()
		c.conn = nil
		return err
	}
	return nil
}

// UDPClient communicates with remote SRR endpoints over UDP.
type UDPClient struct {
	conn        *net.UDPConn
	sessionID   uint32
	nextTransID uint64
}

// DialUDP creates an SRR UDP client targeted at host:port.
func DialUDP(remoteAddr string) (*UDPClient, error) {
	raddr, err := net.ResolveUDPAddr("udp", remoteAddr)
	if err != nil {
		return nil, err
	}

	conn, err := net.DialUDP("udp", nil, raddr)
	if err != nil {
		return nil, err
	}

	return &UDPClient{
		conn:        conn,
		sessionID:   uint32(time.Now().Unix()),
		nextTransID: 1,
	}, nil
}

// Call sends a request datagram and waits for the matching transaction ID reply.
func (c *UDPClient) Call(serviceID, opID uint16, payload []byte, timeout time.Duration) ([]byte, error) {
	tid := atomic.AddUint64(&c.nextTransID, 1) - 1

	dgram := PackUDPDatagram(MsgRequest, serviceID, opID, c.sessionID, tid, payload)
	if _, err := c.conn.Write(dgram); err != nil {
		return nil, err
	}

	deadline := time.Now().Add(timeout)
	buf := make([]byte, 65535)

	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return nil, errors.New("srr: udp call timed out")
		}

		_ = c.conn.SetReadDeadline(time.Now().Add(remaining))
		n, err := c.conn.Read(buf)
		if err != nil {
			return nil, err
		}

		hdr, replyPayload, err := UnpackUDPDatagram(buf[:n])
		if err != nil {
			continue // skip corrupted or invalid datagrams
		}

		if hdr.TransID == tid {
			if hdr.MsgType == MsgError {
				return nil, fmt.Errorf("%w: %s", ErrRemoteService, string(replyPayload))
			}
			return replyPayload, nil
		}
	}
}

// Pulse sends a UDP pulse datagram.
func (c *UDPClient) Pulse(serviceID, opID uint16, code uint32, value uint64) error {
	body := make([]byte, 16)
	binary.LittleEndian.PutUint32(body[0:4], code)
	binary.LittleEndian.PutUint32(body[4:8], 0)
	binary.LittleEndian.PutUint64(body[8:16], value)

	dgram := PackUDPDatagram(MsgPulse, serviceID, opID, c.sessionID, 0, body)
	_, err := c.conn.Write(dgram)
	return err
}

// Close closes the UDP client.
func (c *UDPClient) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}
