# SRR Architecture Specification

SRR is a small, portable C communication library providing synchronous **Send / Receive / Reply (SRR)** messaging with explicit transaction ownership and lifecycle semantics.

---

## 1. Core Abstractions

```text
+-------------------+                    +--------------------+
|  Client Process   |                    |   Server Process   |
|                   |                    |                    |
|  srr_connection_t | ====== IPC ======= |   srr_channel_t    |
|         |         |  (UDS or Pipe)     |         |          |
+---------|---------+                    +---------|----------+
          |                                        |
     srr_send()                               srr_receive()
          |                                        |
          v                                        v
     [Blocks...]                             srr_request_t
          |                                        |
          | <=========== srr_reply() ==============|
          v
     [Unblocks with reply]
```

### 1.1 Channel (`srr_channel_t`)
A server-owned endpoint representing a bounded FIFO work queue.
- Contains receiver waiters (`pthread_cond_t recv_cond`).
- Configurable limits on queue depth, request payload size, and reply payload size.
- Supports multiple receiver threads concurrently dequeuing requests from the same channel.
- Supports dual-mode: in-process direct linkage or background IPC server (UDS or Named Pipe).

### 1.2 Connection (`srr_connection_t`)
A client-side handle representing an active communication link.
- Thread-safe: serializes multiple calling threads per connection via `pthread_mutex_t send_lock`.
- In-process connections hold a reference to `srr_channel_t`.
- IPC connections hold a streaming socket descriptor (POSIX) or file handle (Windows).

### 1.3 Transaction (`srr_request_t`)
Encapsulates a single request/reply lifecycle.
- Owns request payload memory (caller's stack buffer is copied immediately).
- Houses per-transaction condition variable and mutex for unblocking the waiting sender.
- Strictly governs state transitions via single-winner synchronization.

---

## 2. Transaction State Machine & Single-Winner Rules

Every transaction follows a strict single-winner state machine:

```text
                  CREATED
                     |
                     v
                   QUEUED
                     |
                     v
                 DELIVERED
                     |
    +----------------+----------------+
    |                |                |
    v                v                v
 REPLIED        TIMED_OUT        CANCELLED
 (Success)      (Deadline)       (Abandoned)
    |                |                |
    +----------------+----------------+
                     |
                     v
                 TERMINAL
          (Single-winner outcome)
```

### Single-Winner Invariant
A transaction completes **at most once**:
- If `srr_reply()` races against a timeout, only one caller can transition the state to terminal (`state >= SRR_STATE_REPLIED`).
- If the client times out first, the transaction transitions to `SRR_STATE_TIMED_OUT`. When the server subsequently calls `srr_reply()`, it detects the client is no longer waiting and returns `SRR_TIMEOUT` to the server.
- Neither double completions nor use-after-free conditions are possible.

---

## 3. Wire Protocol & Binary Framing

SRR utilizes two distinct framing specifications based on transport requirements (detailed in [docs/PROTOCOL_CONFORMANCE.md](PROTOCOL_CONFORMANCE.md)):

1. **Kernel-Managed IPC Framing (24-byte header):** Used for Unix Domain Sockets and Windows Named Pipes, relying on kernel stream reliability.
2. **Device Wire Framing (32-byte header + CRC-16 + COBS):** Used for packet, serial, and micro-controller transports (see [docs/SRR_WIRE_PROTOCOL.md](SRR_WIRE_PROTOCOL.md)).

### 3.1 IPC Frame Header (24 Bytes)

All cross-process IPC communication uses a packed 24-byte binary frame header:

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

### Message Types (`srr_msg_type_t`)

| Value | Identifier | Purpose |
|:---:|---|---|
| 1 | `SRR_MSG_HELLO` | Protocol handshake (optional) |
| 2 | `SRR_MSG_REQUEST` | Client synchronous request |
| 3 | `SRR_MSG_REPLY` | Server successful reply payload |
| 4 | `SRR_MSG_ERROR` | Server explicit error status |
| 5 | `SRR_MSG_PULSE` | Asynchronous one-way notification |
| 6 | `SRR_MSG_CANCEL` | Client request cancellation |
| 7 | `SRR_MSG_GOODBYE` | Clean connection teardown |

---

## 4. Cross-Platform Transports

SRR abstracts the underlying OS IPC mechanism while presenting an identical API:

| Platform | Transport | Naming Scheme | Mechanism |
|---|---|---|---|
| **In-Process** | `pthread` CV | Direct pointer | Mutex + Condition Variable |
| **POSIX** | Unix Domain Sockets | `/tmp/srr_<name>.sock` | Stream sockets (`AF_UNIX`) |
| **Windows** | Named Pipes | `\\.\pipe\srr_<name>` | `CreateNamedPipeA` / `CreateFileA` |

### Platform Handle Representation
Internal request and connection structures use `intptr_t`:
- On POSIX: casts to `int` socket file descriptor.
- On Windows: casts to `HANDLE`.
This eliminates code duplication in the core transaction logic.

---

## 5. Teardown and Resource Draining

- **Channel Close (`srr_channel_close`):**
  1. Interrupts transport listeners (shuts down listen socket / pipe).
  2. Sets channel state to `SRR_CHAN_CLOSED`.
  3. Broadcasts `recv_cond` to wake all waiting receiver threads with `SRR_CLOSED`.
  4. Flushes queued items, notifying waiting clients with `SRR_SERVER_DIED`.
  5. Shuts down active client connection handles and joins all active worker threads.
- **Channel Destroy (`srr_channel_destroy`):**
  Uses reference counting. Memory is only deallocated when all threads have completely drained their references.
