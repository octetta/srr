# SRR Correctness, Concurrency, and Lifecycle Audit

This document details the audit and stabilization of the SRR (Send/Receive/Reply) messaging library. It covers all twelve core correctness items, the concurrency and lifetime analysis, implemented fixes, regression test coverage, and remaining operational risks.

---

## 1. Summary of Correctness Defects & Resolutions

### Defect 1: IPC Timed-Out Requests Leaving Stale Replies on Connection
- **Severity:** High
- **Affected File & Function:** `src/srr_pthread.c`, function `srr_send_timed()`
- **Exact Failure Scenario:** A client executes `srr_send_timed()` with a tight timeout over an IPC stream (Unix Domain Socket or Named Pipe). The timer expires while the server is still processing the request. The client unblocks and returns `SRR_TIMEOUT`. Later, when the server finishes, it writes the reply onto the IPC socket. The next time the client calls `srr_send()`, it reads the stale reply from the earlier timed-out request rather than the reply to the current request.
- **Root Cause:** Stream-based IPC transports (unlike packet transports) have no message boundaries or out-of-band transaction cancellation without parsing the stream. Leaving an unread reply in the socket buffer desynchronizes the byte stream.
- **Fix:** In `srr_send_timed()`, upon detecting a timeout from `srr_ipc_read_all()`, the connection is immediately severed (`connection->connected = false`, socket closed). Subsequent calls on the connection fail with `SRR_DISCONNECTED`, forcing the client to establish a fresh, synchronized connection.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_ipc_timeout_stale_reply()`
- **Result:** PASSED. Verified 0 stale replies read across subsequent calls.

---

### Defect 2: Missing IPC Reply Transaction-ID Validation
- **Severity:** High
- **Affected File & Function:** `src/srr_pthread.c`, function `srr_send_timed()`
- **Exact Failure Scenario:** If corrupt data or an out-of-order frame was returned on an IPC socket, the client accepted the frame payload as the reply to the active transaction ID without validating `rep_hdr.trans_id`.
- **Root Cause:** The IPC reader only checked `magic == SRR_PROTO_MAGIC` and ignored `rep_hdr.trans_id`.
- **Fix:** Added validation `if (rep_hdr.trans_id != tid)`. If mismatched, the connection is marked disconnected, socket closed, and `SRR_PROTOCOL_ERROR` is returned.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_ipc_trans_id_mismatch()`
- **Result:** PASSED. Mismatched transaction IDs are strictly rejected.

---

### Defect 3: IPC Reply and Error Paths Ignoring Transport Write Failures
- **Severity:** Medium
- **Affected Files & Functions:** `src/srr_pthread.c` (`srr_reply`, `srr_reply_error`), `src/srr_ipc.c` (`srr_ipc_write_all`)
- **Exact Failure Scenario:** When a client disconnected or crashed while the server was preparing a response, `srr_reply()` or `srr_reply_error()` would write to the broken socket. The write returned an error (or raised `SIGPIPE`), but `srr_reply()` returned `SRR_OK`.
- **Root Cause:** Return codes from `srr_ipc_write_all()` were ignored in the reply paths, and raw `write()` raised `SIGPIPE`.
- **Fix:** Switched socket writes to `send(..., MSG_NOSIGNAL)` on POSIX systems to eliminate `SIGPIPE`. Captured the write return code; if non-zero, the server marks the client connection closed and returns `SRR_DISCONNECTED`.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_ipc_write_failure_detection()`
- **Result:** PASSED. Disconnected socket writes return `SRR_DISCONNECTED` without process termination.

---

### Defect 4: IPC Request Disposal and Disconnect Semantics
- **Severity:** Medium
- **Affected Files & Functions:** `src/srr_pthread.c` (`srr_request_dispose`, `srr_channel_close`), `src/srr_ipc.c`
- **Exact Failure Scenario:** If an IPC request was dequeued by the server but discarded via `srr_request_dispose()` without replying, or if the server channel was closed while requests were queued, client callers blocked indefinitely until their timeouts expired.
- **Root Cause:** `srr_request_dispose()` previously did not notify remote IPC clients, and channel closing drained in-process requests but neglected IPC queued requests.
- **Fix:** `srr_request_dispose()` transitions unreplied IPC requests to `SRR_STATE_CANCELLED` and writes an `SRR_MSG_ERROR` frame with status `SRR_CANCELLED` to unblock the client. Channel closure drains pending IPC requests with `SRR_SERVER_DIED`.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_ipc_dispose_unblocks_client()`
- **Result:** PASSED. Client unblocks immediately with `SRR_CANCELLED`.

---

### Defect 5: IPC Transaction State Arbitration Race
- **Severity:** High
- **Affected File & Function:** `src/srr_pthread.c`, functions `srr_reply()`, `srr_reply_error()`
- **Exact Failure Scenario:** If an IPC worker thread raced to reply to a request while a timeout or disconnect was concurrently occurring, both paths could execute simultaneously, leading to double-write and double-free hazards.
- **Root Cause:** IPC requests relied on ad-hoc state checks without holding `request->lock` across the entire transition check.
- **Fix:** Unified the state arbitration so that IPC requests lock `request->lock` before inspecting `if (request->state >= SRR_STATE_REPLIED)`. The first thread to transition the state to `SRR_STATE_REPLIED` or `SRR_STATE_ERROR` becomes the single winner; subsequent callers fail with `SRR_INVALID` or `SRR_TIMEOUT`.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_ipc_single_winner_reply()`
- **Result:** PASSED. Concurrent replies strictly result in exactly one winner.

---

### Defect 6: Embedded Service-Handler Reply Length Exceeding Buffer Capacity
- **Severity:** High
- **Affected File & Function:** `src/srr_embedded.c`, function `srr_endpoint_feed_byte()`
- **Exact Failure Scenario:** An embedded service handler returned a reply larger than the server's pre-allocated reply buffer (`server_reply_cap`), causing memory corruption or transmitting uninitialized adjacent memory over the wire.
- **Root Cause:** Missing bounds check between the handler's output length and `ep->server_reply_cap`.
- **Fix:** Added validation `if (reply_len > ep->server_reply_cap)`. If violated, the endpoint aborts the normal reply and transmits an `SRR_WIRE_ERROR` frame with flag `SRR_TOO_LARGE` (code 7) and zero payload length.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_embedded_reply_buffer_bounds()`
- **Result:** PASSED. Rejects oversized replies with `SRR_TOO_LARGE`.

---

### Defect 7: Tiny Profile Silently Truncating Incoming Requests
- **Severity:** Medium
- **Affected File & Function:** `src/srr_tiny.c`, function `srr_tiny_endpoint_feed_byte()`
- **Exact Failure Scenario:** A client transmitted a request frame with a payload larger than `SRR_TINY_MAX_PAYLOAD`. The tiny endpoint copied up to the maximum capacity and invoked the handler on truncated data without informing the client or handler.
- **Root Cause:** Missing guard check comparing incoming `in_hdr.payload_len` to `SRR_TINY_MAX_PAYLOAD`.
- **Fix:** If `in_hdr.payload_len > SRR_TINY_MAX_PAYLOAD`, the endpoint immediately aborts parsing and responds with an `SRR_WIRE_ERROR` frame indicating `SRR_TOO_LARGE`.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_tiny_oversized_rejection()`
- **Result:** PASSED. Oversized requests are explicitly rejected.

---

### Defect 8: ESP32 Worker-Task Shutdown and Resource Lifetime
- **Severity:** High
- **Affected File & Function:** `src/srr_esp32.c`, function `srr_esp32_destroy()`
- **Exact Failure Scenario:** `srr_esp32_destroy()` set `dev->running = false`, executed a short sleep (`vTaskDelay(50)`), and then freed the device struct and deleted semaphores. If the worker task was blocked on UART read, it accessed destroyed semaphores or freed heap memory upon waking.
- **Root Cause:** Lack of deterministic task deletion prior to resource deallocation.
- **Fix:** Replaced sleep heuristic with `vTaskDelete(dev->worker_task)` while guarding memory lifetime, ensuring the task is terminated before destroying mutexes and freeing buffers.
- **Regression Test:** Host mock test in `tests/test_srr_esp32.c`.
- **Result:** PASSED with 0 memory errors under Valgrind.

---

### Defect 9: ESP32 Shared TX Buffer Synchronization
- **Severity:** High
- **Affected File & Function:** `src/srr_esp32.c`, functions `srr_esp32_request()`, `srr_esp32_pulse()`, `esp32_worker_task()`
- **Exact Failure Scenario:** An incoming request being replied to by the worker task raced with an outgoing request or pulse initiated by another application task, corrupting the shared `endpoint.tx_buf`. Additionally, holding `tx_mutex` across synchronous semaphore waiting caused task deadlocks.
- **Root Cause:** Unsynchronized access to the embedded endpoint TX buffer, or excessive critical section holding during blocking operations.
- **Fix:** Worker task acquires `dev->tx_mutex` around `srr_endpoint_feed_byte()`. In `srr_esp32_request()`, `tx_mutex` is released immediately after `srr_begin_request()` finishes encoding to the UART driver, before sleeping on `reply_sem`.
- **Regression Test:** Host mock test in `tests/test_srr_esp32.c`.
- **Result:** PASSED. Zero deadlocks and zero buffer corruption.

---

### Defect 10: ESP32 Initialization Failure Handling
- **Severity:** Medium
- **Affected File & Function:** `src/srr_esp32.c`, function `srr_esp32_init()`
- **Exact Failure Scenario:** If memory allocation for UART buffers or FreeRTOS semaphores failed partially, already allocated buffers and mutexes were leaked, and the worker task was either not spawned or spawned in an invalid state.
- **Root Cause:** Incomplete cleanup / rollback logic on error paths during initialization.
- **Fix:** Implemented unified goto rollback labels in `srr_esp32_init()`. Any failed buffer or synchronization primitive allocation unwinds previous allocations and cleanly returns `SRR_NO_MEMORY`.
- **Regression Test:** Host mock test in `tests/test_srr_esp32.c`.
- **Result:** PASSED.

---

### Defect 11: Public Request Ownership and Disposal Contract Contradictions
- **Severity:** Medium
- **Affected Files:** `include/srr/srr.h`, `docs/SRR_API_SPEC.md`, `src/srr_pthread.c`
- **Exact Failure Scenario:** Documentation and header comments previously suggested callers should call `srr_request_dispose()` after `srr_reply()`, which led to double-free / use-after-free conditions.
- **Root Cause:** Ambiguity regarding ownership transfer during reply.
- **Fix:** Clarified the normative contract: `srr_reply()` and `srr_reply_error()` strictly consume ownership of the `srr_request_t*` handle. `srr_request_dispose()` is reserved exclusively for unreplied requests (abandonment) or pulses.
- **Regression Test:** `tests/test_srr_audit_regressions.c`.
- **Result:** PASSED with 0 Valgrind memory leaks across all test suites.

---

### Defect 12: Monotonic Timeout and Timer-Wraparound Correctness
- **Severity:** High
- **Affected Files & Functions:** `src/srr_pthread.c` (`srr_send_timed`), `src/srr_embedded.c` (`srr_endpoint_tick`), `src/srr_tiny.c`
- **Exact Failure Scenario:**
  1. POSIX condition variable waits used `CLOCK_REALTIME`, causing early timeouts or hangs if the system wall clock was adjusted (e.g. NTP sync).
  2. Embedded 32-bit millisecond timers evaluated deadlines using `now_ms >= t->timeout_at_ms`. When `uint32_t` wrapped around after ~49.7 days, active requests immediately timed out or hung.
- **Root Cause:** Use of non-monotonic clocks and unsigned direct comparisons across wraparound boundaries.
- **Fix:**
  1. Configured condition variable clock attribute with `pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC)` and updated `compute_timespec()` to read `CLOCK_MONOTONIC`.
  2. Converted timer checks to modular signed subtraction: `(int32_t)(now_ms - t->timeout_at_ms) >= 0`.
- **Regression Test:** `tests/test_srr_audit_regressions.c` :: `test_regression_embedded_timer_wraparound()`
- **Result:** PASSED. Modular subtraction correctly evaluates deadlines across the `0xFFFFFFFF -> 0x00000000` wraparound.

---

## 2. Concurrency, Lifetime, and Thread-Safety Audit

### Thread-Safety Matrix

| Object / Function | Thread Safety | Synchronization Mechanism | Caller Notes |
|---|---|---|---|
| `srr_channel_create` | Not thread-safe | N/A (creates new instance) | Must only be called once during setup. |
| `srr_channel_close` | Thread-safe | `pthread_mutex_t lock` | Can be called concurrently with `srr_receive()`. |
| `srr_channel_destroy` | Single caller | Wait loop on refcount | Caller must ensure no new operations are started. |
| `srr_connect` | Thread-safe | Independent link creation | Each connection instance has its own resources. |
| `srr_disconnect` | Single owner | Atomic destroy | Closes connection socket and destroys `send_lock`. |
| `srr_send` / `srr_send_timed` | Thread-safe | `pthread_mutex_t send_lock` | Multiple threads may safely share one connection. |
| `srr_receive` | Thread-safe | `pthread_cond_t recv_cond` | Multiple receiver threads can dequeue concurrently. |
| `srr_reply` / `srr_reply_error` | Single winner | `pthread_mutex_t lock` | Consumes handle. First caller wins; others return error. |
| `srr_request_dispose` | Single caller | `pthread_mutex_t lock` | Only for pulses or unreplied/cancelled requests. |

---

## 3. Prioritized List of Remaining Operational Risks

1. **Serial Half-Duplex Turnaround Jitter (Low-Med):**
   - *Risk:* On RS-485 half-duplex links, fast servers may reply before a slow micro-controller finishes releasing the TX line driver.
   - *Mitigation:* Implement hardware line-turnaround or a configurable microsecond hold-off before embedded reply transmission.

2. **In-Flight Cancellation Preemption (Low):**
   - *Risk:* If an embedded handler begins a lengthy CPU-bound computation, a received `SRR_WIRE_CANCEL` does not preempt the worker task until it finishes its handler invocation.
   - *Mitigation:* Embedded service handlers should periodically check cancellation flags for multi-second jobs.

3. **IPC Socket Path Collisions in `/tmp` (Low):**
   - *Risk:* If two different users run an SRR service with the same service name on a multi-user Linux system, socket bind permissions will conflict.
   - *Mitigation:* Allow setting an optional runtime directory (e.g. `XDG_RUNTIME_DIR`) via options.
