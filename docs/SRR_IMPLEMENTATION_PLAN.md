# SRR Implementation Plan & Roadmap

This document outlines the phased implementation roadmap reconciling the kickoff brief (`writing-block.md`) with the existing codebase.

---

## Stage Progression & Status

| Stage | Milestone Description | Status |
|:---:|---|:---:|
| **Stage 0** | **Design Specifications & Decision Log**<br>Produce the 5 foundational docs, resolve API questions. | **Complete** |
| **Stage 1** | **Portable Core & Pthread Backend**<br>In-process condition-variable sync, bounded queues, single-winner arbitration. | **Complete** |
| **Stage 2** | **Wire Protocol, COBS Framing, CRC & Streaming Parser**<br>Architecture-independent Little-Endian frames, COBS encode/decode, CRC-16. | **In Progress** |
| **Stage 3** | **Serial Transport & Raspberry Pi Pico Profile**<br>UART / USB CDC byte-stream framing, cooperative execution. | **Planned** |
| **Stage 4** | **ESP32 & AVR Tiny Profile**<br>Zero-heap static buffer profiles, measuring RAM and flash. | **Planned** |
| **Stage 5** | **Process IPC Transports**<br>POSIX Unix Domain Sockets and Windows Named Pipes. | **Complete** |
| **Stage 6** | **BEAM & WebAudio Gateway Integrations**<br>Erlang/OTP `gen_server`, port driver, Elixir package, WebAudio UI. | **Complete** |

---

## Immediate Next Focus: Stage 2 (Wire Protocol & COBS Engine)

1. Implement standalone COBS encode/decode: `src/wire/srr_cobs.c` & `include/srr/srr_cobs.h`.
2. Implement standalone CRC-16-CCITT: `src/wire/srr_crc.c` & `include/srr/srr_crc.h`.
3. Implement streaming chunk parser (`srr_wire_parser_t`) in `src/wire/srr_wire.c` & `include/srr/srr_wire.h`.
4. Add comprehensive unit tests in `tests/test_srr_wire.c` validating arbitrary chunking, bit-corruption detection, and zero heap allocations.
