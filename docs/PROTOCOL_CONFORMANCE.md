# SRR Protocol Conformance and Specification Reconciliation

This document formally specifies the two distinct framing protocols utilized across the SRR ecosystem, reconciles existing architectural documentation, audits protocol feature support, and establishes canonical cross-language binary test vectors.

---

## 1. Architectural Separation of Framing Protocols

SRR intentionally defines and maintains **two distinct binary protocols** optimized for different operational environments. They must not be silently merged.

```text
+--------------------------------------------------------------------------------+
|                               SRR SYSTEM ARCHITECTURE                          |
+--------------------------------------------------------------------------------+
|                                                                                |
|  [ 1. Kernel-Managed IPC Stream Framing ]  [ 2. Architecture-Independent Wire] |
|   - Transports: Unix Domain Sockets, Pipes  - Transports: Serial, UART, UDP    |
|   - Framing: Fixed 24-byte header           - Framing: COBS with 0x00 delimiter|
|   - Integrity: Kernel stream guarantee      - Integrity: CRC-16-CCITT (False)  |
|   - Multiplexing: Per-socket connection     - Multiplexing: Service & Op IDs   |
+--------------------------------------------------------------------------------+
```

### 1.1 Protocol Comparison

| Dimension | 24-Byte IPC Header (`src/srr_proto.h`) | 32-Byte Wire Header (`include/srr/srr_wire.h`) |
|---|---|---|
| **Primary Domain** | Local OS Inter-Process Communication | Microcontrollers, serial buses, UDP networks |
| **Transports** | Unix Domain Sockets (`/tmp/srr_*.sock`), Windows Named Pipes | UART, USB CDC, RS-485, Raw UDP Datagrams |
| **Frame Header Size** | Fixed 24 bytes | Fixed 32 bytes + 2 bytes CRC-16 |
| **Byte Ordering** | Explicit Little-Endian | Explicit Little-Endian |
| **Boundary Delimiting** | Stream length field (`payload_len`) | COBS encoding terminated by `0x00` (or UDP packet boundary) |
| **Error Detection** | Managed by kernel stream integrity | CRC-16-CCITT (False) with polynomial `0x1021` |
| **Routing / Addressing** | File system socket path or pipe name | 16-bit `service_id` and 16-bit `operation_id` |
| **Session Tracking** | Kernel socket connection descriptor | 32-bit `session_id` |

---

## 2. Binary Layouts and Field Definitions

### 2.1 24-Byte IPC Frame Layout (`srr_frame_header_t`)

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic: 0x53525231 ("SRR1")                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Version (1)          |        Message Type           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Flags (0)                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                    Transaction ID (64-bit)                    +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Payload Bytes (Length bytes) ...            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 2.2 32-Byte Device Wire Frame Layout (`srr_wire_header_t`)

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

---

## 3. Protocol Feature Implementation Audit

| Feature | Defined in Spec | Implemented in Runtime | Implementation Status & Behavior |
|---|---|---|---|
| **HELLO (Handshake)** | Yes (Msg Type 1) | No (Reserved) | Connection setup relies on OS transport (socket/pipe) or static UART configuration. Dynamic capability negotiation is intentionally unsupported to keep embedded footprints minimal. |
| **REQUEST** | Yes (Msg Type 2) | Yes | Fully supported across all transports and language adapters. |
| **REPLY** | Yes (Msg Type 3) | Yes | Fully supported. Follows single-winner semantics and releases transaction ownership. |
| **ERROR** | Yes (Msg Type 4) | Yes | Carries explicit numeric error status codes. Unblocks waiting clients immediately. |
| **PULSE** | Yes (Msg Type 5) | Yes | Fire-and-forget asynchronous notification. Does not allocate a reply transaction slot. |
| **CANCEL** | Yes (Msg Type 6) | Partial | Timeouts and server request disposal send `SRR_MSG_ERROR(SRR_CANCELLED)`. Wire parser detects cancel frames, but does not preempt long-running in-flight CPU tasks. |
| **GOODBYE** | Yes (Msg Type 7) | Yes | Supported on IPC disconnects (`srr_disconnect` transmits GOODBYE before closing file descriptor). |
| **Session Validation** | Yes (`session_id`) | Yes | Embedded endpoints validate session IDs across transactions to detect node reboots. |
| **Duplicate Handling** | Yes | Yes | Filtered by transaction ID matching in pending tables and single-winner locks. |

---

## 4. Cross-Language Conformance Matrix

All language implementations must agree byte-for-byte on the canonical test vectors defined in `tests/conformance/vectors.json`. Verification status is tracked transparently based on host environment availability and automated CI pipeline runs:

| Target Implementation | Verification Environment | CRC-16 Vector | COBS Vector | IPC Framing | Wire Framing | Invalid Rejection |
|---|---|:---:|:---:|:---:|:---:|:---:|
| **C Reference Implementation** | Local Host + CI | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** |
| **Python Adapter** | Local Host + CI | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** |
| **Perl Adapter** | Local Host + CI | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** |
| **Go Adapter** | Local Host + CI | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** | **VERIFIED** |
| **Node.js Adapter** | Automated CI (GitHub Actions) | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** |
| **Ruby Adapter** | Automated CI (GitHub Actions) | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** |
| **PHP Adapter** | Automated CI (GitHub Actions) | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** | **VERIFIED in CI** |

> [!NOTE]
> On the local developer host (Fedora without `node`, `ruby`, or `php` installed), `python3 tests/conformance/test_conformance.py` explicitly marks missing interpreters as `UNVERIFIED on current host`. In the GitHub Actions CI environment where all interpreters are provisioned, 100% of adapters are verified automatically.

---

## 5. Canonical Test Vectors Reference

From `tests/conformance/vectors.json`:

1. **CRC-16-CCITT (False):**
   - `"123456789"` $\rightarrow$ `0x29B1` (`10673`)
   - `"SRR1"` $\rightarrow$ `0x1426` (`5158`)
   - Empty input $\rightarrow$ `0xFFFF` (`65535`)

2. **COBS Encoding:**
   - Raw `00` $\rightarrow$ Encoded `0101`
   - Raw `00 01 00 00 02 03 04 00 05 00` $\rightarrow$ Encoded `0102010104020304020501`

3. **Wire Request Frame with COBS & 0x00 Delimiter:**
   - Header: `service=42`, `op=7`, `session=1001`, `tid=999999`, Payload=`"hello_srr"`
   - Canonical Hex:
     `06315252530102020101022a0207010103e90301043f420f01010101020901010c68656c6c6f5f737272ce3e00`
