# SRR Parallax Propeller P1 Multi-Cog Architecture

This document describes the multi-cog execution model and shared Hub RAM integration for the **Parallax Propeller 1 (P8X32A)** microcontroller, implementing the requirements of **Section 10 and Section 17 (Stage 6)** of the Kickoff Brief.

---

## 1. Architectural Principles

The Parallax Propeller 1 consists of:
- **8 Symmetric 32-bit RISC Cogs**, each with 512 x 32-bit (2 KB) local Cog RAM for instructions and local variables.
- **32 KB Shared Hub RAM**, accessed in round-robin sequence (1 access every 16 clock cycles, or 200 ns at 80 MHz).
- **8 Hardware Lock Bits** (`LOCKNEW`, `LOCKSET`, `LOCKCLR`, `LOCKRET`) centrally managed by the Hub controller for mutually exclusive access.
- **Zero Operating System / Zero Interrupts:** Real-time tasks are assigned dedicated cogs rather than software interrupt service routines.

Instead of emulating POSIX threads or forcing cooperative single-threaded main-loops, SRR on P1 implements a **Shared Hub-Memory Ring Buffer & Cog Mailbox Handoff** model:

```text
 Physical UART Pins (RX/TX)
             |
             v
+--------------------------------------------------------+
| Cog 0: Dedicated Full-Duplex Serial Driver             |
|   - Streams incoming bytes into Hub RX Ring Buffer     |
|   - Transmits outgoing frames from Hub TX Ring Buffer  |
+--------------------------------------------------------+
             |                              ^
     Hub RX Ring Buffer             Hub TX Ring Buffer
             |                              |
             v                              |
+--------------------------------------------------------+
| Cog 0 Step / Parser: Streaming COBS & CRC-16 Check     |
|   - Decodes wire frames from Hub RX Ring               |
|   - Populates Hub Request Mailbox                      |
|   - Encodes completed Replies from Mailbox to TX Ring  |
+--------------------------------------------------------+
             |                              ^
     Hub Request Mailbox            Hub Reply Mailbox
   (State: REQUEST_READY)         (State: REPLY_READY)
             |                              |
             +-------------+----------------+
                           |
             +-------------+-------------+
             |                           |
             v                           v
+-------------------------+ +-------------------------+
| Cog 1: Motor Control    | | Cog 2: ADC / Sensors    |
|   - Claims Service 10   | |   - Claims Service 20   |
|   - Executes & Replies  | |   - Emits Alarm Pulses  |
+-------------------------+ +-------------------------+
```

---

## 2. Hub Memory Layout (`srr_p1_hub_t`)

The shared Hub memory structure is declared in [`include/srr/srr_p1_hub.h`](file:///home/stewartj/book/srr/include/srr/srr_p1_hub.h):

```c
typedef struct {
    uint32_t          magic;        /* "SRR1" */
    uint32_t          session_id;   /* Reboot detection */
    uint8_t           lock_id;      /* Hub hardware lock 0..7 */

    /* UART RX/TX Circular Buffers */
    volatile uint32_t rx_head, rx_tail;
    uint8_t           rx_ring[256];
    volatile uint32_t tx_head, tx_tail;
    uint8_t           tx_ring[256];

    /* Work Mailbox for Cog Dispatch */
    srr_p1_mailbox_t  mailbox;

    /* Zero-heap parser scratchpad */
    srr_wire_parser_t parser;
    uint8_t           parser_rx[128];
    uint8_t           parser_dec[128];
    uint8_t           tx_scratch[128];
} srr_p1_hub_t;
```

### Memory Footprint in Hub RAM
- Total size of `srr_p1_hub_t`: **~1.1 KB** of the 32 KB Hub RAM (< 3.5% of total memory).
- Local Cog RAM usage in Cog 0: Under 1 KB (leaves room for full PASM UART driver).
- Worker Cogs require **zero local buffer allocation**; they read and write directly into Hub RAM using standard `rdbyte` / `wrbyte` or C pointer operations.

---

## 3. Cog Coordination Lifecycle

1. **Host Request Arrival:**
   - Host sends a standard COBS-framed request over UART.
   - Cog 0 UART driver pushes incoming bytes into `hub.rx_ring`.
   - `srr_p1_io_cog_step()` streams bytes into `srr_wire_parser_feed_byte()`.
   - On frame validation (CRC-16 match), Cog 0 loads the payload into `hub.mailbox` and sets `mailbox.state = SRR_P1_MBOX_REQUEST_READY`.
2. **Worker Cog Claim:**
   - Worker Cog polls `srr_p1_worker_claim(&hub, cog_id, service_id, ...)`.
   - If a request matching `service_id` is ready, the worker atomically transitions the state to `SRR_P1_MBOX_PROCESSING` and claims the payload.
3. **Execution & Reply:**
   - Worker executes the operation and calls `srr_p1_worker_reply(&hub, cog_id, trans_id, reply_data, len, false)`.
   - Mailbox transitions to `SRR_P1_MBOX_REPLY_READY`.
4. **Wire Framing & Outbound Transmission:**
   - Cog 0 detects `REPLY_READY`, encodes the 32-byte header + reply payload + CRC-16 into COBS, appends delimiter `0x00`, and pushes bytes into `hub.tx_ring`.
   - Cog 0 UART driver streams `hub.tx_ring` out to the host.
5. **Asynchronous Pulses:**
   - Any cog (e.g. Cog 2 on GPIO emergency trip) can call `srr_p1_emit_pulse()` to push an unsolicited pulse directly into `hub.tx_ring` without waiting for a request.
