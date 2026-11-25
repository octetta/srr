package srr

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
)

// Handler processes synchronous requests and one-way pulses.
type Handler interface {
	// HandleRequest is called for each synchronous call.
	// Returning a non-nil error sends an SRR_MSG_ERROR reply to the client.
	HandleRequest(req []byte) ([]byte, error)

	// HandlePulse is called when a non-blocking one-way pulse is received.
	HandlePulse(code uint32, value uint64)
}

// HandlerFunc allows using plain functions as request handlers.
type HandlerFunc func(req []byte) ([]byte, error)

type defaultHandler struct {
	fn HandlerFunc
}

func (d defaultHandler) HandleRequest(req []byte) ([]byte, error) {
	return d.fn(req)
}

func (d defaultHandler) HandlePulse(code uint32, value uint64) {}

// Server is a high-performance native SRR service handling concurrent IPC connections.
type Server struct {
	socketPath string
	handler    Handler
	listener   net.Listener
	wg         sync.WaitGroup
	running    int32
	connsMu    sync.Mutex
	conns      map[net.Conn]struct{}
}

// NewServer creates a new SRR server bound to the given service name or socket path.
func NewServer(serviceName string, handler Handler) *Server {
	socketPath := serviceName
	if !strings.HasPrefix(serviceName, "/") {
		socketPath = fmt.Sprintf("/tmp/srr_%s.sock", serviceName)
	}

	return &Server{
		socketPath: socketPath,
		handler:    handler,
		conns:      make(map[net.Conn]struct{}),
	}
}

// NewServerFunc creates an SRR server with a simple request handler function.
func NewServerFunc(serviceName string, fn HandlerFunc) *Server {
	return NewServer(serviceName, defaultHandler{fn: fn})
}

// ListenAndServe begins listening for incoming client connections.
func (s *Server) ListenAndServe() error {
	// Clean up stale socket file if it exists
	_ = os.Remove(s.socketPath)

	l, err := net.Listen("unix", s.socketPath)
	if err != nil {
		return fmt.Errorf("srr: listen failed on %s: %w", s.socketPath, err)
	}
	s.listener = l
	atomic.StoreInt32(&s.running, 1)

	for {
		conn, err := l.Accept()
		if err != nil {
			if atomic.LoadInt32(&s.running) == 0 {
				return nil // Clean shutdown
			}
			return err
		}

		s.connsMu.Lock()
		s.conns[conn] = struct{}{}
		s.connsMu.Unlock()

		s.wg.Add(1)
		go s.handleConnection(conn)
	}
}

func (s *Server) handleConnection(conn net.Conn) {
	defer func() {
		s.connsMu.Lock()
		delete(s.conns, conn)
		s.connsMu.Unlock()
		conn.Close()
		s.wg.Done()
	}()

	hdrBuf := make([]byte, IPCHeaderSize)

	for {
		// Read 24-byte IPC Header
		if _, err := io.ReadFull(conn, hdrBuf); err != nil {
			return // Client disconnected or connection closed
		}

		hdr, err := DecodeIPCHeader(hdrBuf)
		if err != nil {
			return // Malformed frame
		}

		var payload []byte
		if hdr.PayloadLen > 0 {
			payload = make([]byte, hdr.PayloadLen)
			if _, err := io.ReadFull(conn, payload); err != nil {
				return
			}
		}

		switch hdr.MsgType {
		case MsgRequest:
			reply, err := s.handler.HandleRequest(payload)
			var respFrame []byte
			if err != nil {
				respFrame = EncodeIPCError(hdr.TransID, err.Error())
			} else {
				respFrame = EncodeIPCReply(hdr.TransID, reply)
			}
			if _, err := conn.Write(respFrame); err != nil {
				return
			}

		case MsgPulse:
			if len(payload) >= 16 {
				code := binary.LittleEndian.Uint32(payload[0:4])
				val := binary.LittleEndian.Uint64(payload[8:16])
				s.handler.HandlePulse(code, val)
			}

		case MsgGoodbye:
			return // Orderly client disconnect

		default:
			// Ignore unsupported message types
		}
	}
}

// Shutdown gracefully stops the server, closes the listener, and removes the socket file.
func (s *Server) Shutdown() error {
	if atomic.CompareAndSwapInt32(&s.running, 1, 0) {
		var err error
		if s.listener != nil {
			err = s.listener.Close()
		}

		s.connsMu.Lock()
		for conn := range s.conns {
			_ = conn.Close()
		}
		s.connsMu.Unlock()

		s.wg.Wait()
		_ = os.Remove(s.socketPath)
		return err
	}
	return nil
}
