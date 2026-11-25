package srr

import (
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	WireMagic   uint32 = 0x53525231 // "SRR1" (Little-Endian ASCII)
	WireVersion uint16 = 1

	MsgHello   uint16 = 1
	MsgRequest uint16 = 2
	MsgReply   uint16 = 3
	MsgError   uint16 = 4
	MsgPulse   uint16 = 5
	MsgCancel  uint16 = 6
	MsgGoodbye uint16 = 7

	IPCHeaderSize  = 24
	WireHeaderSize = 32
	CRCSize        = 2
)

var (
	ErrTruncatedHeader = errors.New("srr: header truncated")
	ErrInvalidMagic    = errors.New("srr: invalid protocol magic")
	ErrInvalidVersion  = errors.New("srr: unsupported protocol version")
	ErrCRCMismatch     = errors.New("srr: crc-16 integrity check failed")
	ErrDatagramShort   = errors.New("srr: datagram too short")
	ErrRemoteService   = errors.New("srr: remote service error")
)

// IPCHeader is the 24-byte header for Unix Domain Socket communication.
type IPCHeader struct {
	Magic      uint32
	Version    uint16
	MsgType    uint16
	Flags      uint32
	TransID    uint64
	PayloadLen uint32
}

// WireHeader is the 32-byte architecture-independent wire header for UDP/Serial transports.
type WireHeader struct {
	Magic       uint32
	Version     uint16
	MsgType     uint16
	Flags       uint16
	ServiceID   uint16
	OperationID uint16
	Reserved    uint16
	SessionID   uint32
	TransID     uint64
	PayloadLen  uint32
}

// CRC16 computes CRC-16-CCITT (False): Poly 0x1021, Init 0xFFFF, no reflect.
// Matches native C srr_crc16("123456789") == 0x29B1.
func CRC16(data []byte, initCRC uint16) uint16 {
	crc := initCRC
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if (crc & 0x8000) != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// COBSEncode encodes data using Consistent Overhead Byte Stuffing.
func COBSEncode(data []byte) []byte {
	out := []byte{1}
	codeIdx := 0
	var code byte = 1

	for _, b := range data {
		if b == 0 {
			out[codeIdx] = code
			code = 1
			codeIdx = len(out)
			out = append(out, 1)
		} else {
			out = append(out, b)
			code++
			if code == 0xFF {
				out[codeIdx] = code
				code = 1
				codeIdx = len(out)
				out = append(out, 1)
			}
		}
	}
	out[codeIdx] = code
	return out
}

// COBSDecode decodes data using Consistent Overhead Byte Stuffing.
func COBSDecode(data []byte) ([]byte, error) {
	if len(data) == 0 {
		return []byte{}, nil
	}
	out := make([]byte, 0, len(data))
	i := 0
	n := len(data)
	for i < n {
		code := data[i]
		i++
		if code == 0 {
			return nil, errors.New("srr: zero byte inside COBS data")
		}
		for j := byte(1); j < code; j++ {
			if i >= n {
				return nil, errors.New("srr: truncated COBS stream")
			}
			out = append(out, data[i])
			i++
		}
		if code < 0xFF && i < n {
			out = append(out, 0)
		}
	}
	return out, nil
}

// EncodeWireFrame encodes a COBS-framed wire packet terminated by 0x00 delimiter.
func EncodeWireFrame(msgType uint16, serviceID, opID uint16, sessionID uint32, transID uint64, payload []byte, flags uint16) []byte {
	raw := make([]byte, WireHeaderSize+len(payload)+CRCSize)
	binary.LittleEndian.PutUint32(raw[0:4], WireMagic)
	binary.LittleEndian.PutUint16(raw[4:6], WireVersion)
	binary.LittleEndian.PutUint16(raw[6:8], msgType)
	binary.LittleEndian.PutUint16(raw[8:10], flags)
	binary.LittleEndian.PutUint16(raw[10:12], serviceID)
	binary.LittleEndian.PutUint16(raw[12:14], opID)
	binary.LittleEndian.PutUint16(raw[14:16], 0) // reserved
	binary.LittleEndian.PutUint32(raw[16:20], sessionID)
	binary.LittleEndian.PutUint64(raw[20:28], transID)
	binary.LittleEndian.PutUint32(raw[28:32], uint32(len(payload)))
	copy(raw[WireHeaderSize:], payload)

	crc := CRC16(raw[:WireHeaderSize+len(payload)], 0xFFFF)
	binary.LittleEndian.PutUint16(raw[WireHeaderSize+len(payload):], crc)

	enc := COBSEncode(raw)
	return append(enc, 0x00)
}

// DecodeWireFrame decodes a COBS-framed wire packet terminated by 0x00 delimiter.
func DecodeWireFrame(frame []byte) (*WireHeader, []byte, error) {
	if len(frame) == 0 || frame[len(frame)-1] != 0x00 {
		return nil, nil, errors.New("srr: wire frame must end with 0x00 delimiter")
	}
	unCobs, err := COBSDecode(frame[:len(frame)-1])
	if err != nil {
		return nil, nil, err
	}
	if len(unCobs) < WireHeaderSize+CRCSize {
		return nil, nil, ErrDatagramShort
	}
	body := unCobs[:len(unCobs)-CRCSize]
	receivedCRC := binary.LittleEndian.Uint16(unCobs[len(unCobs)-CRCSize:])
	calcCRC := CRC16(body, 0xFFFF)
	if receivedCRC != calcCRC {
		return nil, nil, ErrCRCMismatch
	}

	magic := binary.LittleEndian.Uint32(body[0:4])
	if magic != WireMagic {
		return nil, nil, ErrInvalidMagic
	}
	ver := binary.LittleEndian.Uint16(body[4:6])
	if ver != WireVersion {
		return nil, nil, ErrInvalidVersion
	}
	plen := binary.LittleEndian.Uint32(body[28:32])
	payload := body[WireHeaderSize:]
	if uint32(len(payload)) != plen {
		return nil, nil, errors.New("srr: payload length mismatch")
	}

	hdr := &WireHeader{
		Magic:       magic,
		Version:     ver,
		MsgType:     binary.LittleEndian.Uint16(body[6:8]),
		Flags:       binary.LittleEndian.Uint16(body[8:10]),
		ServiceID:   binary.LittleEndian.Uint16(body[10:12]),
		OperationID: binary.LittleEndian.Uint16(body[12:14]),
		Reserved:    binary.LittleEndian.Uint16(body[14:16]),
		SessionID:   binary.LittleEndian.Uint32(body[16:20]),
		TransID:     binary.LittleEndian.Uint64(body[20:28]),
		PayloadLen:  plen,
	}
	return hdr, payload, nil
}

// EncodeIPCRequest encodes a synchronous request frame.
func EncodeIPCRequest(transID uint64, payload []byte) []byte {
	buf := make([]byte, IPCHeaderSize+len(payload))
	binary.LittleEndian.PutUint32(buf[0:4], WireMagic)
	binary.LittleEndian.PutUint16(buf[4:6], WireVersion)
	binary.LittleEndian.PutUint16(buf[6:8], MsgRequest)
	binary.LittleEndian.PutUint32(buf[8:12], 0) // flags
	binary.LittleEndian.PutUint64(buf[12:20], transID)
	binary.LittleEndian.PutUint32(buf[20:24], uint32(len(payload)))
	copy(buf[IPCHeaderSize:], payload)
	return buf
}

// EncodeIPCReply encodes a synchronous successful reply frame.
func EncodeIPCReply(transID uint64, payload []byte) []byte {
	buf := make([]byte, IPCHeaderSize+len(payload))
	binary.LittleEndian.PutUint32(buf[0:4], WireMagic)
	binary.LittleEndian.PutUint16(buf[4:6], WireVersion)
	binary.LittleEndian.PutUint16(buf[6:8], MsgReply)
	binary.LittleEndian.PutUint32(buf[8:12], 0)
	binary.LittleEndian.PutUint64(buf[12:20], transID)
	binary.LittleEndian.PutUint32(buf[20:24], uint32(len(payload)))
	copy(buf[IPCHeaderSize:], payload)
	return buf
}

// EncodeIPCError encodes an error reply frame.
func EncodeIPCError(transID uint64, errMsg string) []byte {
	payload := []byte(errMsg)
	buf := make([]byte, IPCHeaderSize+len(payload))
	binary.LittleEndian.PutUint32(buf[0:4], WireMagic)
	binary.LittleEndian.PutUint16(buf[4:6], WireVersion)
	binary.LittleEndian.PutUint16(buf[6:8], MsgError)
	binary.LittleEndian.PutUint32(buf[8:12], 0)
	binary.LittleEndian.PutUint64(buf[12:20], transID)
	binary.LittleEndian.PutUint32(buf[20:24], uint32(len(payload)))
	copy(buf[IPCHeaderSize:], payload)
	return buf
}

// EncodeIPCPulse encodes an asynchronous one-way pulse frame.
func EncodeIPCPulse(code uint32, value uint64) []byte {
	buf := make([]byte, IPCHeaderSize+16)
	binary.LittleEndian.PutUint32(buf[0:4], WireMagic)
	binary.LittleEndian.PutUint16(buf[4:6], WireVersion)
	binary.LittleEndian.PutUint16(buf[6:8], MsgPulse)
	binary.LittleEndian.PutUint32(buf[8:12], 0)
	binary.LittleEndian.PutUint64(buf[12:20], 0)
	binary.LittleEndian.PutUint32(buf[20:24], 16)

	binary.LittleEndian.PutUint32(buf[IPCHeaderSize:IPCHeaderSize+4], code)
	binary.LittleEndian.PutUint32(buf[IPCHeaderSize+4:IPCHeaderSize+8], 0) // reserved
	binary.LittleEndian.PutUint64(buf[IPCHeaderSize+8:IPCHeaderSize+16], value)
	return buf
}

// EncodeIPCGoodbye encodes an orderly disconnect frame.
func EncodeIPCGoodbye() []byte {
	buf := make([]byte, IPCHeaderSize)
	binary.LittleEndian.PutUint32(buf[0:4], WireMagic)
	binary.LittleEndian.PutUint16(buf[4:6], WireVersion)
	binary.LittleEndian.PutUint16(buf[6:8], MsgGoodbye)
	return buf
}

// DecodeIPCHeader decodes a 24-byte IPC frame header.
func DecodeIPCHeader(raw []byte) (IPCHeader, error) {
	if len(raw) < IPCHeaderSize {
		return IPCHeader{}, ErrTruncatedHeader
	}
	magic := binary.LittleEndian.Uint32(raw[0:4])
	if magic != WireMagic {
		return IPCHeader{}, fmt.Errorf("%w: got 0x%X", ErrInvalidMagic, magic)
	}
	ver := binary.LittleEndian.Uint16(raw[4:6])
	if ver != WireVersion {
		return IPCHeader{}, fmt.Errorf("%w: got %d", ErrInvalidVersion, ver)
	}

	return IPCHeader{
		Magic:      magic,
		Version:    ver,
		MsgType:    binary.LittleEndian.Uint16(raw[6:8]),
		Flags:      binary.LittleEndian.Uint32(raw[8:12]),
		TransID:    binary.LittleEndian.Uint64(raw[12:20]),
		PayloadLen: binary.LittleEndian.Uint32(raw[20:24]),
	}, nil
}

// PackUDPDatagram packs a 32-byte header + payload + CRC-16 datagram.
func PackUDPDatagram(msgType, serviceID, opID uint16, sessionID uint32, transID uint64, payload []byte) []byte {
	bodyLen := WireHeaderSize + len(payload)
	dgram := make([]byte, bodyLen+CRCSize)

	binary.LittleEndian.PutUint32(dgram[0:4], WireMagic)
	binary.LittleEndian.PutUint16(dgram[4:6], WireVersion)
	binary.LittleEndian.PutUint16(dgram[6:8], msgType)
	binary.LittleEndian.PutUint16(dgram[8:10], 0) // flags
	binary.LittleEndian.PutUint16(dgram[10:12], serviceID)
	binary.LittleEndian.PutUint16(dgram[12:14], opID)
	binary.LittleEndian.PutUint16(dgram[14:16], 0) // reserved
	binary.LittleEndian.PutUint32(dgram[16:20], sessionID)
	binary.LittleEndian.PutUint64(dgram[20:28], transID)
	binary.LittleEndian.PutUint32(dgram[28:32], uint32(len(payload)))

	copy(dgram[WireHeaderSize:], payload)

	crc := CRC16(dgram[:bodyLen], 0xFFFF)
	binary.LittleEndian.PutUint16(dgram[bodyLen:], crc)
	return dgram
}

// UnpackUDPDatagram unpacks and verifies an incoming UDP datagram.
func UnpackUDPDatagram(dgram []byte) (WireHeader, []byte, error) {
	if len(dgram) < WireHeaderSize+CRCSize {
		return WireHeader{}, nil, ErrDatagramShort
	}

	bodyLen := len(dgram) - CRCSize
	expectedCRC := binary.LittleEndian.Uint16(dgram[bodyLen:])
	calcCRC := CRC16(dgram[:bodyLen], 0xFFFF)
	if expectedCRC != calcCRC {
		return WireHeader{}, nil, ErrCRCMismatch
	}

	magic := binary.LittleEndian.Uint32(dgram[0:4])
	if magic != WireMagic {
		return WireHeader{}, nil, ErrInvalidMagic
	}
	ver := binary.LittleEndian.Uint16(dgram[4:6])
	if ver != WireVersion {
		return WireHeader{}, nil, ErrInvalidVersion
	}

	plen := binary.LittleEndian.Uint32(dgram[28:32])
	if int(plen) != len(dgram)-WireHeaderSize-CRCSize {
		return WireHeader{}, nil, errors.New("srr: payload length mismatch")
	}

	hdr := WireHeader{
		Magic:       magic,
		Version:     ver,
		MsgType:     binary.LittleEndian.Uint16(dgram[6:8]),
		Flags:       binary.LittleEndian.Uint16(dgram[8:10]),
		ServiceID:   binary.LittleEndian.Uint16(dgram[10:12]),
		OperationID: binary.LittleEndian.Uint16(dgram[12:14]),
		Reserved:    binary.LittleEndian.Uint16(dgram[14:16]),
		SessionID:   binary.LittleEndian.Uint32(dgram[16:20]),
		TransID:     binary.LittleEndian.Uint64(dgram[20:28]),
		PayloadLen:  plen,
	}

	payload := dgram[WireHeaderSize:bodyLen]
	return hdr, payload, nil
}
