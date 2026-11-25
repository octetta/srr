# SRR Verification & Test Plan

This document establishes the test strategy and test matrix required across all SRR backends.

---

## 1. Test Categories

### A. Core Concurrency & Race Tests
- **Reply vs Timeout:** Client deadline expires at the exact moment server attempts `srr_reply()`. Verified single-winner resolution.
- **Channel Close with Blocked Receivers:** `srr_channel_close()` safely unblocks all sleeping receiver threads with `SRR_CLOSED`.
- **Channel Close with Blocked Senders:** Undelivered queued transactions resolve with `SRR_SERVER_DIED`.
- **Multi-Client / Multi-Server Stress:** High-throughput stress test (e.g. 8 clients, 4 servers, 4,000+ transactions) verifying zero deadlocks, zero lost wakeups, and zero memory corruption.

### B. Wire Protocol & Framing Tests
- **COBS Encoding / Decoding:** Round-trip validation across varied payloads (empty, single byte, sequences of zeros, sequences without zeros, 255-byte blocks).
- **CRC-16-CCITT Verification:** Tested against known test vectors (`"123456789"` -> `0x29B1`).
- **Bit Corruption & Noise:** Frame corruption must be detected by CRC and discarded cleanly without parser crash.
- **Chunk Boundary Invariance:**
  - 1 byte per feed.
  - Arbitrary chunk splits (e.g. split in the middle of header or CRC).
  - Multiple complete frames arriving in a single read.
- **Oversized & Malformed Frames:** Frames exceeding buffer capacity or with bad magic numbers must be dropped cleanly.

### C. Cross-Process & Transport Tests
- **POSIX Unix Domain Sockets:** Verified over stream sockets with parent/child processes via `fork()`.
- **Windows Named Pipes:** Verified on Windows (`\\.\pipe\srr_*`) with multi-client worker threads.
- **Abrupt Peer Death:** Client or server process exiting during an active transaction must resolve the peer cleanly without indefinite hangs.

---

## 2. Tooling & Sanitizer Matrix

| Environment | Test Target | Sanitizers |
|---|---|---|
| Linux x86_64 | Standard Tests | Valgrind Memcheck (`--leak-check=full --error-exitcode=1`) |
| Linux x86_64 | Race & Stress Tests | ThreadSanitizer (`-fsanitize=thread`) |
| Linux x86_64 | Wire Protocol Tests | AddressSanitizer (`-fsanitize=address,undefined`) |
| Windows (MinGW) | Win32 Named Pipes | MinGW cross-compilation with `-Wall -Wextra -Werror` |

---

## 3. Embedded Verification
- Host-side execution of COBS framing and parser using identical test vectors.
- Measurement of static BSS/data memory: zero heap allocations in the embedded profile.
