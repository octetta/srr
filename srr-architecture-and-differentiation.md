# SRR: Architecture and Differentiation

**Working title:** SRR — synchronous message passing for portable concurrent systems  
**Status:** Architecture proposal, pre-implementation  
**Version:** 0.1  
**Date:** 2026-11-25

---

## 1. Executive summary

SRR is a proposed small, portable C communication library built around the QNX-style **Send / Receive / Reply (SRR)** interaction:

1. A client sends a request to a service and waits.
2. A server receives the request, processes it, and replies.
3. The client resumes with either the reply or a defined terminal result.

The initial implementation should support two execution domains:

- **Thread-to-thread:** independent pthreads communicate through in-process queues and synchronization.
- **Process-to-process:** independent processes communicate through local IPC transports (Unix-domain sockets on POSIX systems and named pipes on Windows).

The same conceptual API should be used in both cases, while the implementation and its guarantees remain explicit about the boundary being crossed.

SRR is **not novel as a general messaging or RPC concept**. QNX already provides the reference interaction; NNG, ZeroMQ, Git Simple-IPC, and Erlang/OTP cover substantial parts of the problem. The opportunity is narrower and more concrete: provide a small C library whose central abstraction is a *blocking transaction with explicit ownership and lifecycle*, first implemented natively between pthreads and then carried across process boundaries without changing the service programming model.

The project should succeed on correctness, simplicity, portability, and a compelling API—not on claiming to invent IPC.

## 2. Goals

### 2.1 Primary goals

- Make request/reply communication natural for ordinary C code.
- Support multiple clients and multiple server receiver threads.
- Provide a native in-process pthread backend without sockets.
- Add process IPC without changing the public service model.
- Make request ownership, reply completion, timeout, cancellation, and teardown semantics explicit.
- Avoid polling and busy waiting.
- Keep the core small, readable, testable, and dependency-light.
- Make it possible to add language adapters later, especially Erlang/Elixir/Gleam.

### 2.2 Non-goals for the first release

Do not include these in v0.1:

- Machine-to-machine networking.
- Browser/WebSocket or WebTransport support.
- Erlang ports, NIFs, or BEAM distribution integration.
- Shared-memory transport.
- Automatic service discovery.
- A schema compiler or IDL.
- Transparent retries or exactly-once execution claims.
- A general publish/subscribe framework.
- Realtime audio-thread calls.
- A broker, daemon, or mandatory event loop.

These may become later adapters or companion components. They should not complicate the first correctness milestone.

## 3. The core model

SRR has four main objects.

### 3.1 Channel

A **channel** is a server-owned endpoint to which requests are delivered. It has:

- A bounded request queue.
- A set of receiver waiters.
- A lifecycle state (open, closing, closed).
- Configurable queue and message limits.
- Optional pulse delivery.

A channel is not a thread. Any number of server threads may call `receive()` on the same channel.

### 3.2 Connection

A **connection** is a client-side handle referring to a channel or remote service. It owns client-side state and identifies the communication relationship.

A connection may support multiple outstanding requests in the future, but the initial synchronous `send()` API need not require that capability. The implementation must nevertheless use transaction identifiers internally so that later asynchronous and multiplexed APIs are possible.

### 3.3 Transaction

A **transaction** represents one request and its eventual terminal result. It owns the request/reply storage needed to make the API safe.

Suggested state machine:

```text
CREATED
   |
   v
QUEUED
   |
   v
DELIVERED
   |
   +------> REPLIED ------> COMPLETED
   |
   +------> CANCELLED
   |
   +------> TIMED_OUT
   |
   +------> DISCONNECTED
   |
   +------> SERVER_DIED
   |
   +------> ERROR
```

The implementation must define which transitions are legal and make terminal transitions single-winner. A reply racing with a timeout must not complete the same transaction twice.

The exact internal states may differ by backend. The externally observable result codes and ownership rules must not.

### 3.4 Pulse

A **pulse** is a small one-way notification that does not wait for a reply. It is useful for hints, state changes, wakeups, and lightweight events.

Pulses are not a substitute for requests. Their delivery and queueing policy must be explicit. A bounded queue can fill; therefore a pulse send must report whether it was accepted, dropped, or rejected rather than silently promising reliable delivery.

## 4. Proposed C API shape

The following is a design sketch, not a frozen ABI.

```c
typedef struct srr_channel srr_channel_t;
typedef struct srr_connection srr_connection_t;
typedef struct srr_request srr_request_t;

typedef enum {
    SRR_OK = 0,
    SRR_TIMEOUT,
    SRR_CANCELLED,
    SRR_DISCONNECTED,
    SRR_SERVER_DIED,
    SRR_CLOSED,
    SRR_QUEUE_FULL,
    SRR_TOO_LARGE,
    SRR_PROTOCOL_ERROR,
    SRR_NO_MEMORY,
    SRR_INVALID
} srr_status_t;

srr_status_t srr_channel_create(
    const srr_channel_options_t *options,
    srr_channel_t **out_channel);

srr_status_t srr_channel_close(srr_channel_t *channel);
void         srr_channel_destroy(srr_channel_t *channel);

srr_status_t srr_connect(
    const char *service_name,
    const srr_connect_options_t *options,
    srr_connection_t **out_connection);

void srr_disconnect(srr_connection_t *connection);

srr_status_t srr_send(
    srr_connection_t *connection,
    const void *request,
    size_t request_size,
    void *reply,
    size_t reply_capacity,
    size_t *reply_size);

srr_status_t srr_send_timed(
    srr_connection_t *connection,
    const void *request,
    size_t request_size,
    void *reply,
    size_t reply_capacity,
    size_t *reply_size,
    uint32_t timeout_ms);

srr_status_t srr_receive(
    srr_channel_t *channel,
    srr_request_t **out_request);

const void *srr_request_data(const srr_request_t *request);
size_t      srr_request_size(const srr_request_t *request);

srr_status_t srr_reply(
    srr_request_t *request,
    const void *reply,
    size_t reply_size);

srr_status_t srr_reply_error(
    srr_request_t *request,
    srr_status_t error);

srr_status_t srr_pulse(
    srr_connection_t *connection,
    uint32_t code,
    uint64_t value);
```

### API design questions to resolve before implementation

1. **Buffer ownership:** Prefer copying request bytes into transaction-owned storage on `send()`. This gives a clear lifetime and makes process transports natural. A later zero-copy extension can be explicit.
2. **Reply storage:** The caller supplies a reply buffer; the implementation copies the reply into it before `send()` returns. If the reply exceeds capacity, return `SRR_TOO_LARGE` and define whether the required size is reported.
3. **Request lifetime:** A received request remains valid until replied to or explicitly abandoned. The API needs an explicit `srr_request_dispose()` or equivalent for abandoned requests.
4. **Timeout semantics:** A timeout means the caller stopped waiting. It does not necessarily mean the server did not execute the request.
5. **Cancellation:** Cancellation is best-effort once a request has been delivered. The server may already be executing it.
6. **Error replies:** Distinguish transport/library errors from application-level error payloads.
7. **Thread safety:** State precisely which objects can be used concurrently and whether a connection supports simultaneous `send()` calls.
8. **Destruction:** Do not permit use-after-free when a channel closes while sends or receives are outstanding. Either destruction waits for references to drain or the API uses explicit close/release semantics.

Do not finalize the public API until these lifetime rules are written and tested.

## 5. Required semantics and guarantees

This section is the heart of the project. SRR should make these guarantees stronger and clearer than an informal wrapper around a queue or socket.

### 5.1 Synchronous request/reply

For a successful `srr_send()`, the caller does not return until it has received the reply. The caller sees one terminal result: a reply or a defined failure status.

The server receives a request handle and replies through that handle. The request handle identifies exactly one transaction.

### 5.2 Single completion

Each transaction completes at most once. Concurrent reply, timeout, disconnect, and shutdown paths must be synchronized so that only one terminal outcome wins.

### 5.3 Explicit ownership

- The library copies request data before the sending call can return or block in a way that permits caller mutation. The exact point of copy is specified.
- The server reads transaction-owned request bytes, not a pointer into an arbitrary client stack buffer.
- The reply is copied into caller-owned output storage before the sender returns.
- The server cannot retain a request pointer after the request has been completed or disposed.
- No backend may rely on native pointers being meaningful outside the process.

### 5.4 Bounded resource use

Channels have bounded queue depth and maximum message size. The library reports backpressure explicitly. A bounded design is preferable to unbounded allocation under load.

The API should expose enough configuration to size queues for real applications, while keeping defaults conservative.

### 5.5 No polling

The pthread backend uses blocking synchronization primitives. A waiting sender sleeps until its transaction completes or its deadline/cancellation condition is reached. A waiting receiver sleeps until work arrives or the channel closes.

No busy-wait loop or mandatory application event loop is permitted in the core.

### 5.6 Shutdown is a first-class operation

Channel close must wake blocked receivers and resolve queued transactions. Connection disconnect must resolve or detach outstanding work according to documented rules. Destruction must not race into undefined behavior.

A server process disappearing is not the same as a normal application error reply.

### 5.7 Honest timeout and failure semantics

For local thread IPC, a timeout can establish that the sender stopped waiting, but the server may already have received the request.

For process IPC, if the connection breaks, the client may not know whether the server received, started, or completed the operation. Therefore:

- `SRR_TIMEOUT` and `SRR_DISCONNECTED` do not imply that the operation did not happen.
- SRR must not automatically retry non-idempotent requests.
- Applications that need safe retries must use application-level idempotency keys or deduplication.
- SRR must not claim exactly-once execution.

This distinction becomes even more important if network transports are added later.

## 6. Architecture

Keep the architecture layered, but avoid creating a generic framework before there are multiple working backends.

```text
                 Public C API
                      |
          Core transaction / ownership
          lifecycle / queue / status model
                      |
          Backend interface (private)
              /               \
       pthread backend     local IPC backend
       in-process          UDS / Windows pipe
              \               /
             platform primitives
```

### 6.1 Public API layer

Owns stable types, status codes, options, and documented object lifetimes. It should not expose socket descriptors, pthread mutexes, Windows handles, or backend-specific structures.

### 6.2 Core transaction layer

Owns:

- Transaction identifiers.
- State transitions.
- Single-completion arbitration.
- Request/reply ownership.
- Deadline handling.
- Queue limits and accounting.
- Error mapping.
- Reference/lifetime management.

The core should be transport-independent, but not at the cost of an elaborate abstraction hierarchy.

### 6.3 pthread backend

Use:

- A mutex protecting channel queue and transaction state.
- Condition variables for receiver waiters and sender completion.
- Per-transaction completion synchronization, or an equally clear mechanism.
- A bounded queue.
- Reference counting or another rigorously specified lifetime scheme.

Avoid one global mutex across all channels. Start with a straightforward per-channel/per-transaction locking design; optimize only after benchmarks demonstrate contention.

The library must not pretend to reproduce QNX kernel scheduling behavior. In particular, portable pthread synchronization does not provide QNX's message-driven server priority inheritance.

### 6.4 Process backend

Initial targets:

- POSIX: Unix-domain stream sockets.
- Windows: named pipes.

Use a dedicated, versioned wire format. Never transmit a native C structure directly.

A minimal frame should include:

| Field | Purpose |
|---|---|
| Magic | Reject unrelated streams |
| Protocol version | Compatibility negotiation |
| Message type | HELLO, REQUEST, REPLY, ERROR, PULSE, CANCEL, GOODBYE |
| Flags | Reserved, explicitly versioned |
| Transaction ID | Correlate reply with request |
| Payload length | Framing and size enforcement |
| Payload | Opaque application bytes |

Define byte order and integer widths. Validate all lengths before allocation or copy. Define maximum frame size and behavior for malformed or unsupported messages.

The process backend will need an I/O mechanism to read and write streams. That does not require exposing an event loop to the application. It may use a small internal I/O thread or a platform-specific wait mechanism. The choice should be made per platform and documented.

### 6.5 Backend boundary

The private backend contract should express only what the core needs: submit a request, deliver a request, deliver a terminal result, send a pulse, and report transport closure.

Do not force the process backend to emulate pointer-based in-memory queue behavior. Both backends must preserve the public semantics, not share implementation details.

## 7. Comparison with existing systems

The comparison below is about the intended first release, not a claim that SRR is superior in every dimension.

### 7.1 QNX Neutrino message passing

QNX is the conceptual reference. It already provides channels and connections, synchronous Send/Receive/Reply, pulses, multiple waiting clients/receivers, and kernel-managed priority behavior. Its message passing can span threads, processes, and networked nodes within the QNX environment.

**What SRR should learn from QNX:**

- The channel/connection model is a good service abstraction.
- Synchronous send/reply makes request ownership and synchronization intuitive.
- Pulses are useful as a distinct lightweight notification mechanism.
- Multiple server receiver threads are a natural scaling model.

**What SRR cannot claim to reproduce:**

- Kernel-integrated scheduling semantics.
- QNX message-driven priority inheritance.
- QNX's kernel-level message delivery and resource-manager integration.
- The same real-time determinism or failure containment.

**Potential SRR distinction:** make the QNX-style programming model available as a portable user-space C library on conventional POSIX and Windows systems, with clearly weaker scheduling guarantees.

### 7.2 NNG

NNG is a serious competitor and may be the right answer for applications that need a mature messaging framework. It provides messaging protocols such as request/reply and transports including in-process, IPC, TCP, TLS, and WebSocket. It separates protocol semantics from transport mechanisms.

**Where NNG is stronger:**

- Broader transport and protocol coverage.
- Existing implementation maturity and documentation.
- TLS and WebSocket support.
- A larger messaging feature set.

**Potential SRR distinction:**

- A deliberately narrow synchronous server API shaped around a received request handle and one explicit reply.
- A native pthread implementation whose primary abstraction is a blocking transaction, not a general messaging socket.
- A small C surface focused on transaction ownership and lifecycle.

This distinction is not automatically unique. NNG already supports request/reply and in-process transport. SRR must demonstrate that its API and lifecycle guarantees are materially simpler or more useful for the intended users.

### 7.3 ZeroMQ

ZeroMQ provides messaging patterns and transports such as inproc, IPC, and TCP. Its design is asynchronous and queue-oriented; for example, a send operation generally queues a message for asynchronous transmission rather than meaning that a remote service has processed and replied.

**Where ZeroMQ is stronger:**

- Rich messaging patterns and ecosystem.
- Flexible topologies and routing.
- Mature asynchronous messaging model.

**Potential SRR distinction:**

- A deliberately synchronous call path: send, block, receive, return.
- Explicit request-handle ownership and terminal transaction states.
- No requirement that an application reason about socket patterns or a broker/proxy topology for the basic service-call case.

Do not characterize ZeroMQ as lacking request/reply. It supports request/reply patterns; the difference is the programming and completion model SRR chooses to emphasize.

### 7.4 Git Simple-IPC

Git's Simple-IPC is a particularly relevant comparison. It provides request/response communication between a client and server using Unix-domain sockets on non-Windows systems and named pipes on Windows. The server uses a thread pool to handle multiple concurrent clients.

**Where Git Simple-IPC is strong:**

- A focused, practical local process request/response implementation.
- Cross-platform local IPC.
- A concrete example of a small application-specific protocol.

**Potential SRR distinction:**

- SRR begins with the same abstraction inside one process between pthreads, then extends it to processes.
- SRR makes channel, connection, request-handle, pulse, timeout, and transaction lifecycle concepts part of the general API.
- SRR is intended as a reusable library and language boundary, rather than a protocol tailored to Git's own application needs.

This is a modest distinction. Git Simple-IPC is evidence that process IPC alone is not a novel reason to build SRR.

### 7.5 Erlang/OTP and BEAM

BEAM already has lightweight processes, message passing, monitors, supervision, and distribution. `GenServer.call` provides synchronous request/reply; `cast` provides asynchronous messaging; delayed replies are supported.

**Where BEAM is stronger:**

- Actor isolation and process lifecycle.
- Supervision and fault recovery.
- Native distribution between BEAM nodes.
- Mature asynchronous concurrency abstractions.

**Potential SRR distinction:**

- SRR is not intended to replace BEAM messaging.
- SRR is a boundary protocol that lets BEAM services communicate with native C services and other non-BEAM participants using a simple request/reply contract.
- A BEAM adapter can map SRR requests onto `GenServer.call` or delayed `GenServer.reply`, while preserving BEAM's own concurrency and supervision model.

The first BEAM adapter should use an external port or local socket gateway, not a blocking NIF. A NIF can be considered later only for a measured need and with careful scheduler-safety constraints.

## 8. What is actually differentiating?

The defensible differentiation is a *combination*, not a single invented primitive:

1. **QNX-inspired synchronous service semantics** in a portable user-space library.
2. **Native pthread-to-pthread operation** as a first-class backend, not merely a socket transport.
3. **A consistent public model** that extends from threads to local processes.
4. **Explicit transaction ownership and terminal lifecycle rules** as part of the API contract.
5. **Small, dependency-light C implementation** with no mandatory broker or application event loop.
6. **A deliberate heterogeneous integration boundary** for BEAM and, later, browser clients.

Items 1, 3, and parts of 4 exist in other systems. Item 2 exists in messaging libraries in different forms. The claim is that SRR puts these choices together around a particularly simple C service-call model.

Before calling this a meaningful differentiator, the project should answer these questions with evidence:

- Is the pthread backend substantially simpler to use than NNG for ordinary synchronous service calls?
- Are request and reply lifetimes easier to reason about than a hand-built condition-variable protocol?
- Can the same server implementation move from in-process to process IPC with only endpoint configuration changes?
- Are shutdown and timeout races demonstrably correct?
- Is the implementation small enough that a systems developer can understand and audit it?

If the answer to these is no, users should probably use NNG or an existing IPC facility.

## 9. Audio and realtime constraints

SRR may be useful in modular audio systems, but synchronous SRR calls must **never be made from a realtime audio callback**.

A blocked audio thread can cause an underrun. Mutexes, condition variables, socket operations, allocation, and unpredictable scheduling are inappropriate in the callback.

A sensible audio architecture is:

```text
UI / controller / sequencer / BEAM service
                 |
              SRR calls
                 |
        synth engine control thread
                 |
       bounded preallocated command queue
                 |
          realtime audio callback
```

SRR can carry configuration, patch changes, voice allocation requests, transport state, diagnostics, and non-realtime orchestration. The engine's control thread validates and translates these into bounded commands for the audio thread. The audio callback consumes those commands using a realtime-safe mechanism.

A useful demonstration could connect a C synthesizer engine, a control/sequencer service, and a browser or BEAM UI. It should prove that SRR is valuable for orchestration without putting SRR in the audio hot path.

## 10. Recommended implementation sequence

### Milestone 0 — contract before code

Write and review:

- Public object model.
- API signatures.
- Buffer ownership rules.
- Transaction state machine.
- Timeout/cancellation semantics.
- Channel close and destruction semantics.
- Thread-safety matrix.
- Queue and message-size limits.

No backend implementation should begin until these are internally consistent.

### Milestone 1 — pthread backend

Implement:

- Channel create/close/destroy.
- Connect/disconnect.
- Blocking send.
- Receive and reply.
- Error reply.
- Timed send.
- Pulses.
- Bounded queues.
- Multiple clients and multiple receiver threads.

Build and test on Linux and macOS first. Keep Windows platform abstraction in view, but do not pretend POSIX pthread APIs are natively portable to Windows.

### Milestone 2 — process backend

Implement:

- Unix-domain socket listener/client.
- Windows named-pipe listener/client.
- Versioned framing.
- Concurrent clients.
- Request/reply correlation.
- Clean disconnect and server shutdown.
- Maximum message size and malformed-frame handling.

The same service handler should be usable with either backend, or the difference should be limited to server/endpoint construction.

### Milestone 3 — evidence

Before expanding scope, publish:

- Correctness and stress-test results.
- Sanitizer runs.
- Cross-platform CI.
- Benchmarks against a simple condition-variable baseline and at least one established library where the comparison is fair.
- A short example that demonstrates the API's advantage.

### Later milestones

Only after the first two backends are stable:

- TCP/TLS machine transport.
- Browser gateway over WebSocket.
- BEAM adapter through a port/socket gateway.
- Optional asynchronous/multiplexed API.
- Service discovery.
- Shared-memory or zero-copy extensions if measurements justify them.

## 11. Required correctness test matrix

The test suite is a core deliverable, not an afterthought.

### Transaction races

- Reply arrives just before timeout.
- Timeout wins just before reply.
- Client disconnect races with reply.
- Server closes channel with requests queued.
- Server closes channel with requests delivered but unanswered.
- Two threads attempt to reply to the same request.
- A request is abandoned or disposed without a reply.
- A late reply arrives after the client has timed out.

### Concurrency and backpressure

- Many clients, one receiver.
- One client, many receivers.
- Many clients, many receivers.
- Queue full behavior.
- Maximum-size request and reply.
- Oversized and malformed messages.
- Repeated connect/disconnect under load.

### Lifecycle and failure

- Client thread exits while blocked in send.
- Receiver thread exits while blocked in receive.
- Channel closes while senders and receivers are blocked.
- Server process exits unexpectedly.
- Client process exits unexpectedly.
- Connection breaks during a frame.
- Partial frame and truncated frame handling.
- Repeated create/destroy cycles under sanitizers.

### Portability

- Linux.
- macOS.
- Windows.
- 32-bit/64-bit assumptions reviewed even if only 64-bit CI is initially available.
- Compiler warnings treated as errors where practical.
- AddressSanitizer and UndefinedBehaviorSanitizer where supported; ThreadSanitizer on supported configurations.

## 12. Benchmarks that matter

Do not publish a single headline throughput number. Measure:

- Median, p95, and p99 round-trip latency for small messages.
- Throughput for small and medium messages.
- One client/one receiver.
- Many clients/one receiver.
- Many clients/many receivers.
- Idle CPU with blocked senders and receivers.
- Allocation count per transaction.
- Queue contention under load.
- Process IPC latency separately from pthread latency.

Compare like with like. An SRR synchronous round trip should not be compared to a ZeroMQ enqueue operation and described as a direct win. Compare complete request/reply transactions and state the test setup.

## 13. Adoption and project positioning

The most credible early audience is:

- C systems programmers who need service-style IPC without building their own synchronization protocol.
- Embedded and desktop systems developers who value a small C API.
- Audio application developers needing a control-plane boundary between components.
- BEAM developers who need to call native services without making native code the center of their architecture.

The likely adoption path is a small, technically sophisticated community, not immediate mass adoption. A clean library, unusually clear lifetime semantics, strong tests, and a compelling working demonstration matter more than a large feature list.

Suggested description:

> **SRR is synchronous message passing for portable concurrent systems.** It brings a QNX-inspired send/receive/reply service model to ordinary C programs, first across threads and then across local processes.

Avoid these claims:

- “The first cross-platform IPC.”
- “A new RPC paradigm.”
- “QNX semantics on every operating system.”
- “Exactly-once delivery.”
- “Realtime-safe IPC.”
- “Faster than NNG/ZeroMQ” without rigorous measurements.

## 14. Build-or-use decision

SRR is worth implementing if the primary goal is to provide a particularly clear synchronous service abstraction for the user's own C systems and to explore a common boundary between C, BEAM, and browser components.

If the goal is simply to ship reliable process messaging today, evaluate NNG and Git Simple-IPC first. If the goal is rich asynchronous messaging patterns, evaluate NNG or ZeroMQ. If the goal is BEAM-to-BEAM distribution, use OTP's native facilities.

The project earns its place only if the pthread backend and the transaction lifecycle contract are genuinely useful enough that developers prefer them over a small custom condition-variable queue or an existing messaging library.

## 15. Decisions to freeze before implementation

The following are the unresolved design decisions that should be settled in a short API review:

1. Is `send()` strictly one outstanding request per connection initially, or can one connection safely have concurrent callers?
2. Is the reply buffer caller-supplied, library-allocated, or both?
3. Does the server explicitly dispose of an unanswered request, or does a request handle automatically error-complete on disposal?
4. Does timeout cancel only the wait, or also send a best-effort cancellation notification to the server?
5. Is channel destruction blocking until outstanding handles are released, or is close/release a two-stage lifecycle?
6. Are pulses delivered through the same `receive()` call as requests, or through a distinct receive API?
7. What are the default queue depth and maximum message size?
8. What exact Windows synchronization and transport primitives are used?

My recommendation is to prefer the simplest semantics that can be implemented and tested rigorously. Avoid speculative generality in the first release.

---

## References

These references support the comparison and describe the existing systems. They are not endorsements or a complete survey.

- **QNX Neutrino IPC:** [Interprocess Communication](https://qnx.com/developers/docs/6.4.0/neutrino/sys_arch/ipc.html) — channels, connections, send/receive/reply queues, pulses, and priority inheritance.
- **QNX priority inheritance:** [Priority inheritance and messages](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.sys_arch/topic/ipc_Priority_inheritance_messages.html).
- **NNG:** [NNG reference manual](https://nng.nanomsg.org/man/v1.3.2/) and [overview](https://nng.nanomsg.org/ref/overview/index.html) — protocol/transport separation and request/reply, in-process, IPC, TCP, TLS, and WebSocket support.
- **ZeroMQ:** [The Guide — Sockets and Patterns](https://zguide.zeromq.org/docs/chapter2/) — messaging patterns and inproc, IPC, and TCP transports.
- **Git Simple-IPC:** [API documentation](https://git-scm.com/docs/api-simple-ipc) — local request/response over Unix-domain sockets and Windows named pipes.
- **Erlang/OTP:** [GenServer documentation](https://www.erlang.org/doc/apps/stdlib/gen_server.html) — synchronous calls, asynchronous casts, and delayed replies.

## Final recommendation

Build **Milestone 0 and Milestone 1 first**. Do not begin by designing a universal network protocol or a browser/BEAM integration framework. Prove that the pthread implementation has a clean, compelling API and that its transaction/lifecycle behavior is correct under adversarial concurrency tests. Then add local process IPC while preserving the contract.

That is the smallest experiment capable of showing whether SRR is more than a reinvention—and the strongest foundation for deciding whether the broader C/BEAM/audio ecosystem idea deserves to exist.
