# SRR Decision Log

This document records the architectural and protocol decisions for SRR, reconciling the original architecture proposal (`srr-architecture-and-differentiation.md`) with the implementation kickoff brief (`writing-block.md`).

---

## 1. Core Architectural Decisions

### Decision 1: Layer Separation
- **Core:** Layer defining types (`srr_channel_t`, `srr_connection_t`, `srr_request_t`), state transitions, timeout semantics, and transaction lifecycles. Zero dependency on sockets, serial, or pthreads.
- **Execution Backends:**
  - `pthread`: Multithreaded in-process desktop/server execution using condition variables.
  - `cooperative / embedded`: Non-blocking, zero-heap, polled execution for bare-metal microcontrollers (AVR, RP2040, ESP32).
- **Transport & Wire:** Independent framing, COBS encoding, CRC integrity, and session handling. Used across serial (UART/USB CDC) and socket/pipe streams.

### Decision 2: The 10 API Questions (from Kickoff Brief §4)

1. **Can a connection have multiple outstanding requests?**
   - *In-Process & Simple IPC:* Serialized per connection (1 outstanding request per connection). Multiple client threads use independent connections or serialize via `conn->send_lock`.
   - *Multiplexed Wire Protocol:* The wire framing includes a 64-bit Transaction ID and Session ID, allowing future asynchronous or multiplexed clients over a single transport stream.
2. **Can multiple server threads receive from the same channel?**
   - *Yes.* Any number of worker threads can invoke `srr_receive(channel, &req)` concurrently. Channel queues are protected by synchronization primitives with condition-variable handoff.
3. **Can a request be replied to exactly once?**
   - *Yes.* Enforced via single-winner state transition. Only the first `srr_reply()` or `srr_reply_error()` completes the transaction. Competing late replies or timeouts return the terminal status and release resources cleanly.
4. **Who owns a delivered request?**
   - The server receiver thread owns the delivered `srr_request_t` handle. The client thread cannot access or mutate request memory once `srr_send()` begins.
5. **When may the request buffer be reused?**
   - The client caller may reuse or free its stack/heap buffer immediately after `srr_send()` copies it (or on `srr_begin_request()` in embedded profiles). The server reads library-owned storage.
6. **What happens if a server closes while clients are blocked?**
   - Blocked receivers unblock with `SRR_CLOSED`. Blocked clients unblock with `SRR_SERVER_DIED`.
7. **What happens if a client disconnects after delivery but before reply?**
   - The server's `srr_reply()` succeeds locally and cleans up, returning `SRR_DISCONNECTED` or `SRR_TIMEOUT` to inform the server that the peer disappeared.
8. **What does timeout mean if the request may already have executed?**
   - `SRR_TIMEOUT` strictly indicates that the *client stopped waiting*. It does *not* imply that the server did not execute or start the operation. Applications must use application-level idempotency tokens if retrying non-idempotent operations.
9. **Can a pulse be dropped when a queue is full?**
   - *Yes.* `srr_pulse()` is non-blocking. If the queue is saturated, it returns `SRR_QUEUE_FULL` (or rejects), making backpressure explicit rather than silently leaking memory.
10. **How are errors represented consistently across backends?**
    - Via the standard enum `srr_status_t` (`SRR_OK` through `SRR_INVALID`). Wire protocol transmits fixed 32-bit status integers.

---

## 2. Wire Protocol Decisions

### Decision 3: Framing via COBS + Zero-Byte Delimiter
- For byte-stream transports (UART serial, USB CDC, sockets), frames are encoded with **Consistent Overhead Byte Stuffing (COBS)** terminated by a `0x00` byte delimiter.
- Guarantees unambiguous frame boundary synchronization even after dropped bytes or line noise.

### Decision 4: CRC-16-CCITT Integrity Check
- Every wire frame includes a 16-bit CRC (polynomial `0x1021`, initial `0xFFFF`) calculated over the unencoded frame bytes.
- Detects corruption over physical serial lines before any message dispatch occurs.

### Decision 5: Architecture-Independent Wire Header
- No compiler-dependent packed structs. All fields are explicitly serialized in **Little-Endian** byte order:
  - Magic (32-bit): `0x53525231` (`"SRR1"`)
  - Version (16-bit): `1`
  - Message Type (16-bit)
  - Flags (16-bit)
  - Session ID (32-bit)
  - Transaction ID (64-bit)
  - Service ID (16-bit)
  - Operation ID (16-bit)
  - Payload Length (32-bit)
  - Header CRC (16-bit)

---

## 3. Embedded & Zero-Allocation Profile Decisions

### Decision 6: Cooperative Execution API
- For bare-metal / microcontroller systems (AVR, RP2040 Pico), provide static/caller-provided buffers:
  - `srr_wire_parser_t`: Streaming chunk parser requiring zero heap allocations.
  - Streaming byte feeder `srr_wire_parse_byte()` / `srr_wire_parse_chunk()`.
