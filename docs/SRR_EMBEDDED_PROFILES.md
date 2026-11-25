# SRR Embedded Profiles & Platform Adapters

This document specifies the resource budgets, design constraints, and build procedures for the resource-constrained MCU targets in SRR, covering **Stage 4** of the implementation roadmap.

---

## 1. Profile Comparison Matrix

| Feature | Full Profile (Linux / POSIX) | Compact Profile (RP2040 Pico) | Tiny Profile (AVR ATmega328P) | RTOS Profile (ESP32) |
|---|:---:|:---:|:---:|:---:|
| **Operating System** | Linux, macOS, Windows | Bare metal / Pico SDK | Bare metal (AVR) | FreeRTOS / ESP-IDF |
| **Concurrency** | POSIX `pthread` / Win32 | Cooperative / Main-loop | Cooperative / Main-loop | FreeRTOS Tasks & Semaphores |
| **Dynamic Allocation** | Optional (at initialization) | **Zero (100% Static)** | **Zero (100% Static)** | FreeRTOS Heap (task stack) |
| **Max Payload Size** | Configurable (up to 64 KB+) | Configurable (e.g. 512 B) | **32 Bytes** | Configurable (e.g. 2 KB) |
| **Max Concurrent Transactions** | Bounded Queue (64+) | 4 Concurrent | **1 Outstanding** | 4 Concurrent |
| **Static RAM Footprint** | ~14 KB | ~2 KB | **~392 Bytes** | ~8 KB |
| **Code Footprint (.text)** | ~18 KB | ~4.1 KB | **~2.8 KB** | ~2.7 KB |
| **Framing & Checksum** | COBS + CRC-16-CCITT | COBS + CRC-16-CCITT | COBS + CRC-16-CCITT | COBS + CRC-16-CCITT |
| **Wire Interoperability** | 100% Interoperable | 100% Interoperable | 100% Interoperable | 100% Interoperable |

---

## 2. AVR Tiny Profile (`srr_tiny.h` / `srr_tiny.c`)

### Hardware Budget
- **Target Device:** Microchip ATmega328P (Arduino Uno, Nano, Pro Mini).
- **RAM Budget:** 2,048 Bytes SRAM total.
  - SRR Tiny Endpoint: **~392 Bytes** (~19% of total RAM).
  - Headroom for user application & stack: **~1,656 Bytes** (~81%).
- **Flash Budget:** 32,768 Bytes Flash total.
  - SRR Wire + Tiny code: **~5.5 KB** (~17% of total Flash).

### Design Rules
1. **Single Outstanding Transaction:** Only one client request may be in-flight at any time. If `srr_tiny_begin_request()` is called while a transaction is pending, it returns `0` (busy).
2. **Payload Clamp:** Application requests or replies exceeding 32 bytes are safely rejected without buffer overflow.
3. **Interrupt-Driven RX:** In AVR applications, incoming bytes from `USART_RX_vect` are pushed into a ring buffer or directly fed via `srr_tiny_feed_byte()`.

### AVR Build & Integration Example
```c
#include "srr_tiny.h"
#include <avr/io.h>

static srr_tiny_endpoint_t g_ep;

static int usart_tx(const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    for (size_t i = 0; i < len; i++) {
        while (!(UCSR0A & (1 << UDRE0)));
        UDR0 = data[i];
    }
    return 0;
}

static int sensor_handler(uint16_t svc, uint16_t op, const uint8_t *req, uint8_t req_len,
                          uint8_t *rep, uint8_t cap, uint8_t *out_len, void *ud) {
    if (op == 1 && cap >= 2) {
        rep[0] = 0x55;
        rep[1] = 0xAA;
        *out_len = 2;
        return 0;
    }
    return -1;
}

int main(void) {
    srr_tiny_init(&g_ep, 0x1234, usart_tx, NULL);
    srr_tiny_register_service(&g_ep, 1, sensor_handler, NULL);

    while (1) {
        if (UCSR0A & (1 << RXC0)) {
            uint8_t b = UDR0;
            srr_tiny_feed_byte(&g_ep, b);
        }
    }
}
```

---

## 3. ESP32 FreeRTOS Adapter (`srr_esp32.h` / `srr_esp32.c`)

### Architecture
The ESP32 adapter brings an RTOS multi-tasking model without polluting the portable SRR core:
1. **UART Worker Task:** A dedicated FreeRTOS task blocks on `uart_read_bytes()` and streams bytes into the non-blocking `srr_endpoint_t`.
2. **TX Mutex:** `dev->tx_mutex` serializes frame transmissions so multiple FreeRTOS tasks can send requests and pulses concurrently over the same physical UART link.
3. **Synchronous Notification:** When a task calls `srr_esp32_request()`, it blocks on `xSemaphoreTake(dev->reply_sem, timeout)` until the worker task receives and matches the corresponding transaction ID.
4. **Host Emulation:** On non-ESP platforms (Linux/macOS), the adapter automatically builds an equivalent POSIX `pthread` and `pipe` model, enabling full testability and Valgrind verification in host CI.

### ESP-IDF Build & Integration Example
In your ESP-IDF `main.c`:
```c
#include "srr_esp32.h"

void app_main(void) {
    srr_esp32_config_t cfg;
    srr_esp32_default_config(&cfg);
    cfg.uart_port = UART_NUM_1;
    cfg.tx_pin = 17;
    cfg.rx_pin = 16;
    cfg.baud_rate = 115200;

    srr_esp32_device_t *srr_dev = NULL;
    srr_esp32_create(&cfg, &srr_dev);

    char reply[64];
    size_t actual_len = 0;
    int rc = srr_esp32_request(srr_dev, 1, 1, "HELLO", 5, reply, sizeof(reply), &actual_len, 1000);
    if (rc == 0) {
        printf("Received reply: %.*s\n", (int)actual_len, reply);
    }
}
```
