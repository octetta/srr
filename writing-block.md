# SRR — Implementation Kickoff Brief for Antigravity

## Mission

Implement SRR as a small, portable C service communication library inspired by QNX Send/Receive/Reply.

The goal is one coherent programming model that works across:

1. Threads within a process.
2. Separate processes on the same host.
3. Linux-to-embedded-device communication over serial.
4. Resource-constrained bare-metal and RTOS systems.
5. Future BEAM and browser integrations.

The architecture and differentiation document supplied separately is the conceptual reference. This brief describes the implementation work and its priorities.

Do not treat this as permission to implement every proposed feature immediately. Build a correct, minimal foundation first.

---

# 1. Before writing code

Inspect the supplied architecture document and the existing repository, if one exists.

Report:

- Existing code, build system, conventions, and dependencies.
- Architectural decisions already established.
- Contradictions, ambiguities, and missing decisions.
- Any proposed API or protocol decisions that would create compatibility problems later.
- A proposed source tree and implementation sequence.

Do not silently invent answers to unresolved architectural questions. Record them in a decision log.

Create these initial documents:

- `docs/SRR_IMPLEMENTATION_PLAN.md`
- `docs/SRR_API_SPEC.md`
- `docs/SRR_WIRE_PROTOCOL.md`
- `docs/SRR_DECISIONS.md`
- `docs/SRR_TEST_PLAN.md`

Keep these documents concise, precise, and implementation-oriented. Avoid duplicating the entire architecture document.

Before substantial implementation, present the API and wire-protocol proposals for review.

# 2. Architectural boundaries

Maintain three distinct layers.

## A. SRR core

The core defines:

- Channels.
- Connections.
- Requests.
- Replies.
- Pulses.
- Transaction identifiers.
- Timeouts and cancellation semantics.
- Error reporting.
- Message ownership and lifetime.
- Service-level communication semantics.

The core must not depend on a particular transport.

## B. Execution backends

Backends implement the core semantics using the facilities available on each platform.

Initial backends:

- POSIX pthreads.
- POSIX process IPC, later.
- Embedded cooperative execution, later.

Do not force embedded systems to emulate pthreads.

## C. Transport and wire protocol

The wire layer defines how messages cross a process or device boundary.

Initial transports:

- Unix-domain sockets, after the local backend.
- UART serial.
- USB CDC, potentially using the same byte-stream framing.

The pthread backend should not serialize messages unnecessarily.

The wire protocol must not depend on native C structure layouts, alignment, packing, or endianness.

Keep these boundaries explicit in the source tree and documentation.

# 3. Language and implementation constraints

Primary implementation language: C99.

C11 features may be used behind clearly documented portability boundaries when useful, but do not make them mandatory for the embedded core.

Requirements:

- No mandatory third-party dependencies.
- No mandatory dynamic allocation in the embedded profile.
- Fixed-width integer types for protocol fields.
- Explicit byte serialization and deserialization.
- No compiler-specific packed structures in the wire protocol.
- No hidden global state.
- Bounded queues and buffers.
- Explicit ownership and lifetime rules.
- No busy waiting.
- No mandatory event loop in the pthread API.
- No polling-based implementation of blocking receive.
- No blocking operations in interrupt handlers.
- No unbounded queues.
- No implicit retries of non-idempotent operations.

Prefer straightforward code over abstraction for abstraction's sake.

Avoid introducing a generic object system, plugin framework, complicated build generator, or large dependency tree.

# 4. Define the public API

Propose and document a minimal public API before implementing it.

The API should cover these concepts:

- Channel creation and destruction.
- Client connection and disconnection.
- Sending a request.
- Receiving a request.
- Replying successfully.
- Replying with an error.
- Sending a one-way pulse.
- Timed operations.
- Cancellation where supported.

Use the architecture document's proposed API names as a starting point:

```c
srr_channel_create()
srr_channel_close()
srr_channel_destroy()

srr_connect()
srr_disconnect()

srr_send()
srr_send_timed()

srr_receive()

srr_reply()
srr_reply_error()

srr_pulse()
```

These are proposed names, not an instruction to preserve an incomplete API unchanged.

For each function, specify:

- Parameters and return values.
- Blocking behavior.
- Timeout behavior.
- Thread-safety.
- Ownership of input and output buffers.
- Whether the operation can allocate memory.
- What happens during concurrent shutdown.
- What happens if a peer disappears.

Resolve the following questions explicitly:

1. Can a connection have multiple outstanding requests?
2. Can multiple server threads receive from the same channel?
3. Can a request be replied to exactly once?
4. Who owns a delivered request?
5. When may the request buffer be reused?
6. What happens if a server closes while clients are blocked?
7. What happens if a client disconnects after delivery but before reply?
8. What does timeout mean if the request may already have executed?
9. Can a pulse be dropped when a queue is full?
10. How are errors represented consistently across backends?

Do not make the public API transport-specific.

# 5. Transaction lifecycle and concurrency correctness

Implement an explicit transaction state machine.

At minimum, account for:

```text
CREATED
  |
QUEUED
  |
DELIVERED
  |
REPLIED
  |
COMPLETED
```

Terminal alternatives:

```text
CANCELLED
TIMED_OUT
DISCONNECTED
SERVER_DIED
ERROR
```

Define legal transitions and enforce them.

Requirements:

- Exactly one terminal outcome per transaction.
- No double reply.
- No use-after-free.
- No request buffer reuse while a receiver still owns it.
- No lost wakeups.
- No deadlocks during shutdown.
- No condition-variable waits without checking their predicates.
- No unbounded waiting during destruction.
- Correct behavior when timeout races with reply.
- Correct behavior when disconnect races with delivery.

Use mutexes and condition variables for the initial pthread implementation.

Do not use polling or busy loops.

Do not claim QNX-equivalent priority inheritance unless it is actually implemented and verified.

# 6. First implementation: pthread backend

This is the first implementation milestone.

Implement:

- Channel creation.
- Multiple client connections.
- Multiple server receiver threads.
- Bounded request queues.
- Blocking receive.
- Blocking request/reply.
- Timed request/reply.
- One-way pulses.
- Channel closure and orderly shutdown.
- Correct concurrent lifetime management.

Use a small example demonstrating:

- One server.
- Several clients.
- Several receiver threads.
- Concurrent requests.
- Successful replies.
- Timeouts.
- Shutdown with blocked clients and receivers.

The implementation should be usable without an application-level event loop.

Do not begin process IPC until the pthread backend has passed its correctness tests.

# 7. Wire protocol specification

Design a versioned, architecture-independent binary protocol.

The initial protocol should support:

- HELLO / capability negotiation.
- REQUEST.
- REPLY.
- ERROR.
- PULSE.
- CANCEL, if supported.
- GOODBYE / orderly disconnect.

Every request/reply exchange must have an unambiguous transaction identifier.

Define:

- Protocol version.
- Message type.
- Flags.
- Session identifier.
- Transaction identifier.
- Service identifier.
- Operation identifier.
- Payload length.
- Payload encoding.
- Integrity check.

Specify byte order explicitly.

Specify maximum frame and payload sizes.

Specify behavior for:

- Invalid frames.
- Truncated frames.
- Oversized frames.
- Unsupported protocol versions.
- Unknown message types.
- Unknown services and operations.
- Duplicate transaction identifiers.
- Stale replies.
- Reconnection after device reset.

Do not serialize native C structures.

## Serial framing

Use COBS framing with a zero-byte delimiter as the initial candidate.

Use a CRC to detect accidental corruption.

Document the CRC algorithm and test vectors.

CRC is not authentication or encryption.

The parser must tolerate arbitrary input chunk boundaries. A read operation may return one byte, half a frame, or several frames.

The framing implementation must not assume that a UART read corresponds to one message.

# 8. Serial transport prototype

After the wire format is specified, implement a Linux serial transport.

Initially support:

- UART serial devices.
- Configurable baud rate.
- Full-duplex communication.
- Bounded RX and TX buffers.
- Frame parsing.
- CRC verification.
- Transaction multiplexing.
- Timeout handling.
- Link loss detection.
- Reconnection and session establishment.

Use a clear separation between:

1. Byte transport.
2. Frame encoding and decoding.
3. SRR message dispatch.
4. Transaction management.
5. Service implementation.

The serial backend must not require changes to the wire protocol when changing from UART to USB CDC.

Do not implement SPI, CAN, RS-485, or network transports in this milestone.

# 9. Embedded implementation profile

The embedded core must be usable without pthreads, POSIX, an operating system, or dynamic allocation.

Provide a cooperative execution model.

The embedded API should allow an application to:

- Feed received bytes into the parser.
- Extract complete messages.
- Dispatch requests.
- Produce replies.
- Advance pending transactions.
- Extract outgoing frames.
- Handle timeout progression using a caller-provided clock.

Do not force an embedded application to block while waiting for a reply.

Provide asynchronous transaction operations such as:

```c
srr_begin_request()
srr_poll_transaction()
srr_collect_reply()
srr_cancel_transaction()
```

These are illustrative names. Define the final API consistently.

Use caller-provided storage or statically allocated buffers.

The protocol and framing code should compile independently of the pthread implementation.

# 10. Hardware targets

Design the common protocol and execution model with these targets in mind:

| Target | Initial profile |
|---|---|
| Linux | Full |
| ESP32 | Full / Compact |
| Raspberry Pi Pico / RP2040 | Compact |
| AVR Arduino Uno | Tiny |
| Parallax Propeller P1 | Specialized Tiny |

Do not implement all targets at once.

Recommended sequence:

1. Linux serial loopback.
2. Raspberry Pi Pico cooperative endpoint.
3. ESP32 / FreeRTOS endpoint.
4. AVR Arduino endpoint.
5. Parallax P1 endpoint.

## Resource profiles

Treat these as initial design budgets, not immutable protocol limits.

**Full profile**

- Multiple outstanding transactions.
- Multiple services.
- Configurable bounded queues.
- Larger payloads.
- RTOS or native threads where available.

**Compact profile**

- Fixed buffers.
- No mandatory heap allocation.
- Limited outstanding transactions.
- Cooperative or RTOS execution.
- Modest payload sizes.

**Tiny profile**

- One outstanding request initially.
- Very small payloads.
- No heap allocation.
- Main-loop dispatch.
- Minimal state and fixed buffers.

For AVR, begin with a 16–32 byte application payload target and measure actual RAM and flash consumption.

For RP2040, begin with a modest fixed-buffer configuration and report actual memory use.

For ESP32, support an RTOS-based execution model without making FreeRTOS types part of the public portable API.

For P1, investigate its shared-memory and cog execution model before proposing the implementation. Do not assume POSIX or conventional MCU threading.

# 11. Capability negotiation and session management

Implement capability negotiation before relying on device-specific assumptions.

At link establishment, exchange:

- Protocol version.
- Maximum frame size.
- Maximum payload size.
- Maximum outstanding transactions.
- Supported message types.
- Supported capabilities.
- Device identity and firmware version, where available.

A device reboot must create a new session.

Transaction identifiers from a previous session must not be mistaken for current transactions.

Define duplicate-request handling.

Do not promise exactly-once execution.

If duplicate suppression is implemented, define its memory bounds, lifetime, and behavior after reboot.

A timeout does not prove that the remote operation did not execute.

Never automatically retry a non-idempotent operation without an explicit application policy.

# 12. Demonstration application

The first end-to-end demonstration should be Linux communicating with a Raspberry Pi Pico.

Implement a small device service with operations such as:

```text
device.info
gpio.read
gpio.write
adc.read
counter.get
counter.reset
```

The Pico should also be able to originate an event, delivered to Linux as an SRR pulse.

The Linux application should demonstrate:

1. Opening the serial connection.
2. Establishing an SRR session.
3. Discovering device capabilities.
4. Sending requests.
5. Receiving replies.
6. Receiving device-originated events.
7. Handling timeout.
8. Handling unplug or link loss.
9. Reconnecting after the device restarts.

Keep the demonstration small. Its purpose is to validate the architecture, not become a device-management framework.

# 13. Testing requirements

Testing is a first-class deliverable.

## Core tests

- Channel creation and destruction.
- Connect and disconnect.
- Request and reply.
- Multiple clients.
- Multiple receivers.
- Concurrent requests.
- Queue full.
- Timeout.
- Cancellation.
- Pulse delivery.
- Double reply.
- Server shutdown.
- Client shutdown.
- Concurrent shutdown.
- Invalid arguments.
- Buffer ownership and lifetime.

## Concurrency tests

Exercise:

- Reply versus timeout races.
- Disconnect versus delivery races.
- Channel close versus blocked receive.
- Channel close versus blocked send.
- Multiple simultaneous replies.
- Repeated creation and destruction.
- High-contention workloads.

Run ThreadSanitizer where supported.

Run AddressSanitizer and UndefinedBehaviorSanitizer where supported.

Use deterministic tests for state transitions wherever possible. Stress tests supplement, but do not replace, deterministic correctness tests.

## Protocol tests

- Known-good frame vectors.
- CRC vectors.
- Empty payload.
- Maximum payload.
- Invalid CRC.
- Truncated frame.
- Random noise.
- Repeated delimiters.
- Arbitrary byte chunking.
- Multiple frames in one read.
- Oversized frames.
- Invalid message types.
- Session changes.
- Duplicate transaction identifiers.

Include fuzzing targets for the frame parser and message decoder where tooling is available.

## Embedded tests

The framing and wire protocol must have host-side tests independent of hardware.

Use the same test vectors for Linux and embedded builds.

Report RAM and flash usage for each embedded target.

# 14. Build system and repository organization

Prefer a small, conventional CMake build or an equally simple existing build system.

Do not introduce Python or JavaScript as mandatory build dependencies.

Provide independent build targets for:

- Portable core.
- pthread backend.
- Wire protocol.
- Serial transport.
- Examples.
- Unit tests.
- Embedded profile.

A possible source organization:

```text
include/
    srr/
        srr.h
        srr_types.h
        srr_config.h
        srr_wire.h

src/
    core/
    pthread/
    wire/
    transport/
        serial/
    embedded/

examples/
    pthread/
    serial/
    pico/

tests/
    core/
    pthread/
    wire/
    fuzz/

docs/
```

Adjust this organization if the existing repository has a better established convention.

Do not create unnecessary source-file fragmentation.

# 15. Performance and resource measurements

Measure rather than guess.

For pthreads, report:

- Request/reply latency.
- Throughput.
- Concurrent client behavior.
- Queue-full behavior.
- Memory consumption.
- Thread count.

For serial, report:

- Frame overhead.
- Effective payload throughput at representative baud rates.
- Round-trip latency.
- Maximum supported payload.
- Behavior under dropped, corrupted, and delayed frames.

For embedded targets, report:

- Static RAM.
- Stack requirements.
- Flash/code size.
- Maximum frame buffer.
- Maximum transaction count.
- CPU cost where measurable.

Do not optimize prematurely, but do not leave resource use unmeasured.

# 16. Real-time boundary

SRR is a control-plane communication mechanism.

Never invoke blocking SRR operations from a real-time audio callback, interrupt handler, or other hard-deadline execution context.

For audio and other real-time systems, provide a documented integration pattern:

```text
SRR client/server
       |
       v
bounded preallocated command queue
       |
       v
real-time processing thread
```

The real-time side must never wait for SRR, allocate memory, or acquire a potentially blocking SRR mutex.

The same principle applies to embedded interrupt handlers: ISR code moves bytes or signals work; normal execution context performs SRR processing.

# 17. Implementation stages and acceptance gates

Work in the following order.

## Stage 0 — Design review

Deliver:

- API specification.
- Wire protocol specification.
- Decision log.
- Source organization.
- Test plan.

Acceptance:

- API ownership and lifecycle rules are explicit.
- Transaction state transitions are explicit.
- Wire format is independent of native C layout.
- Embedded execution does not depend on pthreads.
- Unresolved decisions are identified rather than hidden.

## Stage 1 — Portable core and pthread backend

Deliver:

- Compilable C implementation.
- Public headers.
- pthread backend.
- Unit tests.
- Concurrency tests.
- Working examples.

Acceptance:

- All core tests pass.
- No known data races or memory errors.
- Bounded queues work correctly.
- Shutdown is safe with blocked callers.
- No busy waiting.

## Stage 2 — Wire protocol and serial loopback

Deliver:

- Encoder and decoder.
- COBS framing.
- CRC.
- Linux serial backend.
- Loopback test application.
- Protocol test vectors.

Acceptance:

- Arbitrary input chunking works.
- Invalid frames are rejected safely.
- Maximum-size frames work.
- No native-structure serialization.
- No unbounded allocations.

## Stage 3 — Raspberry Pi Pico endpoint

Deliver:

- Cooperative embedded implementation.
- Fixed-buffer configuration.
- Linux-to-Pico demonstration.
- Device-originated pulse.
- Reconnect handling.

Acceptance:

- Same protocol test vectors pass.
- No heap allocation required in the embedded core.
- Resource consumption is documented.
- Device reset and reconnection work.

## Stage 4 — ESP32 and AVR

Deliver:

- ESP32 implementation.
- AVR Tiny profile.
- Memory and flash measurements.
- Platform-specific build instructions.

Acceptance:

- Common protocol interoperability.
- Explicitly documented capability limits.
- No unnecessary platform dependencies in the portable core.

## Stage 5 — Process IPC

Only after the earlier stages are stable:

- Unix-domain socket backend.
- Process-level client/server examples.
- Disconnect and peer-death behavior.
- Transport failure tests.
- Windows backend design and implementation plan.

## Stage 6 — P1, BEAM, browser

Treat these as subsequent integration projects.

Do not let them complicate the initial core or wire protocol.

# 18. Working rules for Antigravity

1. Preserve the architectural boundaries in this brief.
2. Do not implement speculative features just because they appear in the architecture document.
3. Do not silently change public API or wire semantics after tests depend on them.
4. Record design changes in `docs/SRR_DECISIONS.md`.
5. Keep every stage buildable and testable.
6. Do not remove existing functionality without explicit justification.
7. Do not substitute stubs for claimed working features.
8. Do not claim tests passed unless they were actually run.
9. Report platform limitations honestly.
10. Prefer a small, complete implementation over a large, partially implemented framework.

At the end of each stage, provide:

- Files added or changed.
- Important design decisions.
- Build commands.
- Test commands and actual results.
- Sanitizer results, where applicable.
- Known limitations.
- Remaining work.
- A concise assessment of whether the stage's acceptance criteria are met.

Do not proceed past a stage's acceptance gate merely because the code compiles.

# 19. First assignment

Start with Stage 0.

Inspect the architecture document and repository.

Produce the five design documents listed in Section 1.

Pay particular attention to the API and wire protocol decisions that affect all future implementations.

Do not begin a broad implementation until these specifications are internally consistent.

The immediate objective is to give SRR a stable foundation that can be implemented once and carried from Linux pthreads to a small cooperative MCU without forcing every platform to behave like a desktop operating system.

**Success means a small, understandable, dependable C library—not the largest possible feature set.**