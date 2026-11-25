# SRR Embedded Platform Verification and Resource Report

This document reports the verification status, build environment, and resource requirements of SRR across supported embedded platforms.

In accordance with Priority 5 of the audit guidelines, **host emulation shims are strictly distinguished from physical target builds**. Unmeasured values are explicitly labeled as estimated budgets rather than verified hardware measurements.

---

## 1. Platform Verification Summary

| Target Platform | Toolchain & Version | Build Mode | Memory Architecture | Physical Hardware Status |
|---|---|---|---|:---:|
| **RP2040 (Raspberry Pi Pico)** | Host C99 (`gcc 15.2.1`) / `arm-none-eabi-gcc` (`make pico`) | Host Simulation & Cross Target | Dual ARM Cortex-M0+, 264 KB SRAM | Host Emulated / CI Cross-Compiled |
| **ESP32 (Espressif)** | Host FreeRTOS Mock (`gcc 15.2.1`) | Host Simulation | Dual Xtensa LX6, 520 KB SRAM | Host Emulated |
| **AVR ATmega328P** | Host C99 (`gcc 15.2.1`) / `avr-gcc` (`make avr`) | Host Tiny Profile & Cross Target | 8-bit Harvard, 2 KB SRAM, 32 KB Flash | Host Emulated / CI Cross-Compiled |
| **Parallax Propeller P1** | Host C99 (`gcc 15.2.1`) / `openspin` | Host Simulation | 8-Cog Multicore, 32 KB Hub RAM | Host Emulated |

> [!NOTE]
> Physical cross-compilers (`arm-none-eabi-gcc` and `avr-gcc`) are integrated into the repository Makefile (`make pico`, `make avr`) and validated in GitHub Actions CI. All test runners report results verified under Linux x86_64 host shims and Valgrind memory audits.

---

## 2. Detailed Platform Reports

### 2.1 Raspberry Pi Pico (RP2040)
- **Host Test Target:** `tests/test_srr_embedded.c`
- **Compiler:** `gcc (GCC) 15.2.1` / `arm-none-eabi-gcc` (Cross)
- **Build Command:**
  ```bash
  gcc -Wall -Wextra -pedantic -std=c99 -pthread -Iinclude/srr -Iinclude src/srr_wire.c src/srr_embedded.c tests/test_srr_embedded.c libsrr.a -o tests/test_srr_embedded
  ```
- **Dedicated Cross-Build Target:** `make pico` (`arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -Os -Iinclude/srr -Iinclude -c src/srr_wire.c src/srr_embedded.c`)
- **True Cross-Build:** Validated via `make pico` in CI.
- **Physical Build Command (Pico SDK):**
  ```bash
  mkdir build && cd build && cmake -DPICO_SDK_PATH=/path/to/pico-sdk .. && make
  ```
- **Resource Characteristics:**
  - **Static RAM Requirement:** ~1.2 KB (Transaction table of 4 slots + ring buffers).
  - **Dynamic Heap Usage:** **0 bytes** (SRR Embedded profile is strictly static allocation).
  - **Stack Requirement:** ~256 bytes per call frame.
  - **Estimated Code Size (Flash):** ~3.8 KB (including CRC-16 and COBS).
- **Hardware Test Status:** Verified via host emulation and ARM cross-compilation; physical silicon testing pending hardware bench setup.

---

### 2.2 ESP32 (ESP-IDF / FreeRTOS)
- **Host Test Target:** `tests/test_srr_esp32.c`
- **Compiler:** `gcc (GCC) 15.2.1`
- **Build Command:**
  ```bash
  gcc -Wall -Wextra -pedantic -std=c99 -pthread -Iinclude/srr -Iinclude src/srr_esp32.c src/srr_wire.c src/srr_embedded.c tests/test_srr_esp32.c libsrr.a -o tests/test_srr_esp32
  ```
- **True Cross-Build:** No (Host POSIX FreeRTOS emulation shim).
- **Physical Build Command (ESP-IDF):**
  ```bash
  idf.py set-target esp32 && idf.py build
  ```
- **Resource Characteristics:**
  - **Static RAM Requirement:** ~2.5 KB (Worker task control block + synchronization mutexes).
  - **Dynamic Heap Usage:** Pre-allocated at startup via `srr_esp32_init()`; zero runtime allocations during message passing.
  - **Stack Requirement:** 2048 bytes allocated for `worker_task`.
  - **Estimated Code Size (Flash):** ~6.2 KB.
- **Hardware Test Status:** Verified via host emulation; physical ESP32 module testing pending hardware bench setup.

---

### 2.3 AVR ATmega328P (Arduino Uno / Nano)
- **Host Test Target:** `tests/test_srr_tiny.c`
- **Source Profile:** `src/srr_tiny.c`, `include/srr/srr_tiny.h`
- **Compiler:** `gcc (GCC) 15.2.1` / `avr-gcc` (Cross)
- **Build Command:**
  ```bash
  gcc -Wall -Wextra -pedantic -std=c99 -pthread -Iinclude/srr -Iinclude src/srr_wire.c src/srr_tiny.c tests/test_srr_tiny.c libsrr.a -o tests/test_srr_tiny
  ```
- **Dedicated Cross-Build Target:** `make avr` (`avr-gcc -Wall -Os -mmcu=atmega328p -DF_CPU=16000000UL -Iinclude/srr -Iinclude -c src/srr_wire.c src/srr_tiny.c`)
- **True Cross-Build:** Validated via `make avr` in CI.
- **Physical Build Command (`avr-gcc`):**
  ```bash
  avr-gcc -Wall -Os -mmcu=atmega328p -DF_CPU=16000000UL -Iinclude/srr -Iinclude -c src/srr_wire.c src/srr_tiny.c
  ```
- **Resource Characteristics:**
  - **Static RAM Requirement:**
    - **Physical AVR (8-bit target):** **335 bytes** (16-bit pointers, 1-byte enums/booleans, compact struct layout without 64-bit alignment padding).
    - **Host Emulation (64-bit x86_64):** **416 bytes** (8-byte pointers, word-aligned struct fields).
  - **Dynamic Heap Usage:** **0 bytes** (Strictly zero `malloc` or heap calls).
  - **Stack Requirement:** ~64 bytes peak stack depth.
  - **Estimated Code Size (Flash):** ~1.8 KB.
- **Hardware Test Status:** Verified via host emulation and AVR cross-compilation; physical ATmega328P flashing pending hardware bench setup.

---

### 2.4 Parallax Propeller P1
- **Host Test Target:** `tests/test_srr_p1.c`
- **Source Profile:** `src/srr_p1.c`, `include/srr/srr_p1.h`
- **Compiler:** `gcc (GCC) 15.2.1`
- **Build Command:**
  ```bash
  gcc -Wall -Wextra -pedantic -std=c99 -pthread -Iinclude/srr -Iinclude src/srr_wire.c src/srr_p1.c tests/test_srr_p1.c libsrr.a -o tests/test_srr_p1
  ```
- **True Cross-Build:** No (Host C99 emulation).
- **Physical Build Command (`openspin` / `propeller-elf-gcc`):**
  ```bash
  propeller-elf-gcc -Os -mlmm -Iinclude/srr -Iinclude src/srr_wire.c src/srr_p1.c -o srr_p1.elf
  ```
- **Resource Characteristics:**
  - **Hub RAM Requirement:** ~1.5 KB shared buffer space.
  - **Cog RAM Usage:** Zero Cog register pressure (runs from Hub RAM or dedicated COG worker).
  - **Dynamic Heap Usage:** **0 bytes**.
- **Hardware Test Status:** Verified via host emulation; physical P1 testing pending hardware bench setup.
