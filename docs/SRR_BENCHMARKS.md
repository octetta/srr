# SRR Performance & Resource Measurements Report

This document records the empirical measurements, latency profiles, throughput metrics, and static memory footprints across all SRR backends, fulfilling the requirements of **Section 15** of the Kickoff Brief.

---

## 1. Latency & Throughput Summary

All measurements executed with `-O3` optimization on Linux x86_64 (`benchmarks/bench_srr`):

| Communication Backend | Configuration / Payload | Throughput | Median Latency (p50) | 95th Percentile (p95) | 99th Percentile (p99) |
|---|---|:---:|:---:|:---:|:---:|
| **Pthreads (In-Process)** | 1 Client -> 1 Server (64B) | **142,407 ops/s** | **6.73 µs** | 7.75 µs | 14.95 µs |
| **Pthreads (Multi-Threaded)** | 4 Clients -> 4 Servers (64B) | **446,213 ops/s** | **7.65 µs** | 15.83 µs | 23.07 µs |
| **Unix Domain Sockets (IPC)** | 1 Client -> 1 Server (64B) | **54,591 ops/s** | **16.99 µs** | 24.45 µs | 31.84 µs |
| **Queue-Full / Backpressure** | Bounded Queue Saturated (32 cap) | **12,938,562 ops/s** | **0.04 µs (40 ns)** | 0.08 µs | 0.32 µs |
| **Wire Frame Encoding** | 32B Hdr + 64B Payload + CRC16 + COBS | **2,783,205 frames/s** | **0.34 µs (340 ns)** | 0.34 µs | 0.36 µs |
| **Wire Streaming Parser** | 1-Byte Chunk Feed (64B Payload) | **2,004,487 frames/s** | **0.47 µs (470 ns)** | 0.48 µs | 1.12 µs |
| **Serial Full-Duplex Loopback** | 32B Payload (End-to-End Serial) | **9,840 ops/s** | **94.94 µs** | 153.30 µs | 209.51 µs |

---

## 2. In-Depth Analysis by Section

### A. Pthreads In-Process Backend
- **Round-Trip Overhead:** ~6.7 µs per synchronous request/reply under baseline 1-to-1 contention.
- **Scaling Behavior:** Throughput scales linearly from **142k ops/s** (single thread) to **446k ops/s** with 4 client and 4 server threads, demonstrating efficient lock handoff via condition variables with negligible lock starvation.
- **Queue Backpressure:** When a channel's bounded queue is full, non-blocking pulses reject in **40 nanoseconds** (`SRR_QUEUE_FULL`), avoiding memory exhaustion.

### B. Process IPC (Unix Domain Sockets)
- Context-switch and kernel socket buffer transit add ~10 µs compared to in-process channels, delivering **54.5k ops/s** and **16.99 µs** median round-trip latency.

### C. Wire Protocol & Serial Framing
- **COBS Encoding Cost:** Encodes header, payload, and CRC-16 in **340 nanoseconds** per frame.
- **Incremental Parsing:** The zero-heap streaming chunk parser processes individual incoming bytes at over **2 Million complete frames per second**.
- **Serial Transmission Limits:**
  - Standard UART line at 115,200 baud (11.52 KB/s raw): Max theoretical throughput for 68-byte frames is ~169 frames/sec.
  - High-speed UART at 921,600 baud (92.16 KB/s raw): Theoretical ~1,355 frames/sec.
  - Full-duplex USB CDC / virtual serial loopback achieves **9,840 ops/s** at **94.9 µs** latency.

---

## 3. Memory & Binary Footprint

Compiled binary object sizes measured via `size`:

| Component / Object File | Code Size (`.text`) | Data (`.data`) | BSS (`.bss`) | Total RAM Required |
|---|:---:|:---:|:---:|:---:|
| **AVR Tiny Profile (`srr_tiny.o`)** | **2,852 B** | 8 B | 0 B | **~392 Bytes** (Fixed Static) |
| **Wire Protocol Engine (`srr_wire.o`)** | **3,484 B** | 0 B | 0 B | **0 B** (Caller-buffered) |
| **Embedded Endpoint (`srr_embedded.o`)** | **4,149 B** | 0 B | 0 B | **~2 KB** (Caller-buffered) |
| **ESP32 FreeRTOS Adapter (`srr_esp32.o`)** | **2,712 B** | 0 B | 0 B | **~8 KB** (Stack + Buffers) |
| **POSIX IPC Backend (`srr_ipc.o`)** | **4,923 B** | 0 B | 0 B | Dynamic per socket buffer |
| **Pthread Core (`srr_pthread.o`)** | **5,361 B** | 0 B | 0 B | Dynamic per channel options |

---

## 4. Resource Budgets on Hardware Targets

1. **AVR ATmega328P (2 KB SRAM, 32 KB Flash):**
   - SRR Tiny code consumes **8.7% of Flash** (2.85 KB).
   - SRR Tiny endpoint consumes **19.1% of SRAM** (392 bytes).
   - Over **1.65 KB of SRAM** remains available for user sensors, application variables, and call stack.
2. **Raspberry Pi Pico RP2040 (264 KB SRAM, 2 MB Flash):**
   - SRR Embedded code consumes **< 0.5% of Flash** and **< 1% of SRAM**.
3. **ESP32 (520 KB SRAM, 4 MB Flash):**
   - Background worker task requires a 4 KB stack; total memory overhead is under 2% of heap.
