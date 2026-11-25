# SRR Implementation Plan & Roadmap

This document outlines the phased implementation roadmap reconciling the kickoff brief (`writing-block.md`) with the existing codebase.

---

## Stage Progression & Status

| Stage | Milestone Description | Status |
|:---:|---|:---:|
| **Stage 0** | **Design Specifications & Decision Log**<br>Foundational specifications, API behavioral contracts, decision logs. | **Complete** |
| **Stage 1** | **Portable Core & Pthread Backend**<br>In-process condition-variable sync, bounded queues, single-winner arbitration. | **Complete** |
| **Stage 2** | **Wire Protocol, COBS Framing, CRC & Streaming Parser**<br>Architecture-independent Little-Endian frames, COBS encode/decode, CRC-16. | **Complete** |
| **Stage 3** | **Serial Transport & Raspberry Pi Pico Profile**<br>UART / USB CDC byte-stream framing, cooperative execution. | **Complete** |
| **Stage 4** | **ESP32, AVR Tiny & Parallax P1 Profiles**<br>Zero-heap static buffer profiles, measuring RAM and flash. | **Complete** |
| **Stage 5** | **Process IPC Transports**<br>POSIX Unix Domain Sockets and Windows Named Pipes. | **Complete** |
| **Stage 6** | **BEAM, Scripting & WebAudio Gateway Integrations**<br>Erlang/OTP `gen_server`, port driver, Elixir package, WebAudio UI, scripting CLI. | **Complete** |
| **Stage 7** | **Stabilization, Concurrency & Conformance Audit (v0.1.0 - v0.1.1)**<br>TSan race elimination, monotonic condition clocks, IPC stale reply prevention, canonical cross-language test vectors. | **Complete** |

---

## Current Status: Release v0.1.1 (Stabilization & Conformance)

All primary phases from design through embedded deployment, IPC, language adapters, and concurrency stabilization are fully implemented and verified under Valgrind, AddressSanitizer, ThreadSanitizer, and GitHub Actions CI.
