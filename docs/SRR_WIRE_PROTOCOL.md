# SRR Wire Protocol Specification

Version: 1.0  
Status: Implementation Specification  
Byte Order: Little-Endian  

---

## 1. Overview & Framing

The SRR Wire Protocol transmits structured Send/Receive/Reply transactions across byte-stream transports (UART serial, USB CDC, sockets, pipes).

### 1.1 Framing Mechanism: COBS
To ensure robust synchronization over noisy streams without escaping overhead:
1. Every frame payload (header + application payload + CRC) is encoded using **Consistent Overhead Byte Stuffing (COBS)**.
2. The encoded frame is terminated by a single **zero-byte delimiter (`0x00`)**.
3. A zero byte *never* appears inside a valid COBS-encoded packet. Receiving `0x00` immediately signals end-of-frame.

```text
+-----------------------------------+------+
|    COBS-Encoded Frame Payload     | 0x00 |
+-----------------------------------+------+
```

---

## 2. Unencoded Frame Structure

Before COBS encoding (and after COBS decoding), the frame layout is:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic: 0x53525231 ("SRR1")                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Version (1)          |        Message Type           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             Flags             |          Service ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Operation ID         |           Reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Session ID                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                    Transaction ID (64-bit)                    +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Payload Bytes (Length bytes) ...            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            CRC-16             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 2.1 Header Fields

| Offset | Field | Type | Description |
|:---:|---|:---:|---|
| 0 | `magic` | `uint32_t` | Constant `0x53525231` (ASCII `"SRR1"`) |
| 4 | `version` | `uint16_t` | Protocol version (currently `1`) |
| 6 | `msg_type` | `uint16_t` | Message type identifier |
| 8 | `flags` | `uint16_t` | Reserved flags (must be `0`) |
| 10 | `service_id` | `uint16_t` | Target/originating service identifier |
| 12 | `operation_id` | `uint16_t` | Operation identifier within service |
| 14 | `reserved` | `uint16_t` | Reserved padding (must be `0`) |
| 16 | `session_id` | `uint32_t` | Link session ID (detects reboot/reconnect) |
| 20 | `trans_id` | `uint64_t` | Unique transaction correlation ID |
| 28 | `payload_len` | `uint32_t` | Byte length of payload following header |
| 32 | `payload` | `uint8_t[]` | Opaque application payload |
| 32+L | `crc16` | `uint16_t` | CRC-16-CCITT computed over bytes 0 to (32+L-1) |

Total unencoded header size = **32 bytes** + **2 bytes CRC** = 34 bytes minimum frame.

---

## 3. Message Types (`srr_msg_type_t`)

```c
typedef enum {
    SRR_MSG_HELLO    = 1, /* Capability negotiation / session setup */
    SRR_MSG_REQUEST  = 2, /* Synchronous request */
    SRR_MSG_REPLY    = 3, /* Synchronous successful reply */
    SRR_MSG_ERROR    = 4, /* Synchronous error reply */
    SRR_MSG_PULSE    = 5, /* Asynchronous one-way notification */
    SRR_MSG_CANCEL   = 6, /* Best-effort transaction cancellation */
    SRR_MSG_GOODBYE  = 7  /* Orderly session shutdown */
} srr_msg_type_t;
```

---

## 4. Integrity Verification: CRC-16-CCITT

- **Algorithm:** CRC-16-CCITT (False)
- **Polynomial:** `0x1021` ($x^{16} + x^{12} + x^5 + 1$)
- **Initial Value:** `0xFFFF`
- **Input Reflected:** No
- **Result Reflected:** No
- **XOR Output:** `0x0000`

### Known Test Vectors
- String `"123456789"` $\rightarrow$ CRC = `0x29B1`
- String `"SRR1"` $\rightarrow$ CRC = `0x1426`

---

## 5. Streaming Parser & Chunk Invariance

A parser must accept bytes arriving across arbitrary chunk boundaries:
- A single byte per `read()` call.
- Half a frame per call.
- Multiple frames in a single call.

### Parser State Machine
1. **`HUNT`:** Scanning for start of frame (discarding noise until previous delimiter).
2. **`COLLECT`:** Accumulating incoming bytes into buffer until `0x00` delimiter is reached.
3. **`DECODE`:** Decoding COBS in-place.
4. **`VERIFY`:** Validating CRC-16. If invalid, frame is dropped.
5. **`DISPATCH`:** Parsing header fields and delivering to SRR core.
