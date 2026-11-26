# Lightweight Emulators, Virtual Targets & FFI Integration Guide

This guide details how to develop, compile, test, and run SRR across microcontrollers (**AVR Arduino**, **RP2040 Pico**, **ESP32**, and **Parallax Propeller P1**) on **Fedora Linux** without physical hardware and without complex setup.

It also documents **Foreign Function Interface (FFI)** integration for scripting languages loading `libsrr.so` directly.

---

## 1. Zero-Setup Host Emulation (Built-in to SRR)

Before installing any third-party tools, note that **SRR includes native host-level hardware simulations** right inside this repository. They compile with standard host `gcc` and run in milliseconds:

| Target Platform | Simulation File | Emulated Hardware Features | Run Command |
|---|---|---|---|
| **AVR ATmega328P** | [`tests/test_srr_tiny.c`](../tests/test_srr_tiny.c) | 10-bit ADC registers, static 392B RAM, zero-heap engine | `./tests/test_srr_tiny` |
| **RP2040 Pico** | [`examples/pico/pico_device_sim.c`](../examples/pico/pico_device_sim.c) | Dual-core cooperative loop, flash info, GPIO/LED | `./tools/srr_cli --sim info` |
| **ESP32** | [`tests/test_srr_esp32.c`](../tests/test_srr_esp32.c) | FreeRTOS tasks, TX mutexes, queue handles via POSIX threads | `./tests/test_srr_esp32` |
| **Parallax P1** | [`tests/test_srr_p1.c`](../tests/test_srr_p1.c) | 8-Cog Hub RAM round-robin arbiter & hardware lock bits | `./tests/test_srr_p1` |

All four run automatically when you run `make test`.

---

## 2. AVR / Arduino ATmega328P Emulation

### 2.1 `simavr` (Recommended for Fedora)
`simavr` is a lean, lightning-fast C-based AVR simulator that boots in under 10 milliseconds and can route simulated microcontroller UART directly to a Linux pseudo-terminal (`/dev/pts/X`).

#### Installation on Fedora:
```bash
sudo dnf install -y simavr avr-gcc avr-libc avrdude
```

#### Compiling an AVR SRR Firmware:
```bash
avr-gcc -Wall -Os -mmcu=atmega328p -DF_CPU=16000000UL \
    -Iinclude/srr -Iinclude \
    src/srr_tiny.c src/srr_wire.c examples/avr/main.c \
    -o srr_avr_firmware.elf
```

#### Running `simavr` with Virtual UART:
```bash
# Start simulator exposing UART to a pseudo-terminal:
simavr -m atmega328p -f 16000000 srr_avr_firmware.elf
# Output prints: "UART attached to /dev/pts/3"
```

#### Interacting via `srr_cli`:
In another terminal, connect `srr_cli` directly to the simulated Arduino:
```bash
./tools/srr_cli --serial /dev/pts/3 --baud 115200 info
./tools/srr_cli --serial /dev/pts/3 --baud 115200 req 1 1 "PING"
```

### 2.2 `qemu-system-avr`
```bash
sudo dnf install -y qemu-system-avr
qemu-system-avr -machine uno -bios srr_avr_firmware.elf -nographic -serial pty
```

---

## 3. Raspberry Pi Pico (RP2040) Emulation

### 3.1 `rp2040js` (Zero-Compilation, Node-based)
If you already have Node.js installed, `rp2040js` runs directly without compiling emulator source code:
```bash
npx rp2040js --uart-endpoint tcp:8888 build/srr_pico.elf
```
Connect `srr_cli` to the TCP UART port:
```bash
socat pty,link=/tmp/virtual_pico,raw,echo=0 tcp:127.0.0.1:8888 &
./tools/srr_cli --serial /tmp/virtual_pico info
```

### 3.2 `wokwi-cli` (Single Standalone Binary)
Wokwi provides a single-binary CLI runner with zero external dependencies:
```bash
curl -L https://wokwi.com/ci/install.sh | sh
wokwi-cli --diagram examples/pico/diagram.json
```

---

## 4. ESP32 (Wi-Fi & FreeRTOS) Emulation

### 4.1 Espressif Official QEMU (`qemu-system-xtensa`)
Espressif maintains an official QEMU fork supporting ESP32 dual-core Xtensa CPUs, FreeRTOS, and simulated networking.

#### Download Pre-Compiled Binary (No Building Needed):
```bash
curl -LO https://github.com/espressif/qemu/releases/download/esp-develop-20220919/qemu-xtensa-softmmu-esp_develop_20220919-x86_64-linux-gnu.tar.gz
tar -xzf qemu-xtensa-softmmu-*.tar.gz
```

#### Run ESP32 Firmware:
```bash
./qemu-system-xtensa -nographic -machine esp32 \
    -drive file=build/flash_image.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=udp::8888-:8888
```
Now test over UDP from your host terminal:
```bash
./tools/srr_cli --udp 127.0.0.1:8888 ping
```

---

## 5. Parallax Propeller P1: Native Spin + PASM Architecture

For the Parallax Propeller P1, C is often secondary to **Spin** and **PASM (Propeller Assembly)**, which are the native, most expressive languages for the 8-Cog architecture.

### 5.1 Native Spin Objects Provided in SRR
The repository provides clean, production-ready Spin and PASM implementations in [`examples/p1_spin/`](../examples/p1_spin/):

* **[`FullDuplexSerial.spin`](../examples/p1_spin/FullDuplexSerial.spin):** Canonical high-speed PASM UART driver (9,600 to 230,400+ baud) running inside a dedicated Cog, communicating via circular buffers in Hub RAM.
* **[`SIDcog.spin`](../examples/p1_spin/SIDcog.spin):** Dedicated PASM audio synthesis Cog emulating the MOS 6581 sound synthesizer (3 voices, PWM counter DAC audio output on pins P10/P11, reading 29 SID registers from Hub RAM).
* **[`SRR_SID_Server.spin`](../examples/p1_spin/SRR_SID_Server.spin):** Single SID synth server over PASM UART.
* **[`SRR_MultiSID_Server.spin`](../examples/p1_spin/SRR_MultiSID_Server.spin):** Multi-SID server utilizing all 8 Cogs (Cog 0: SRR Router, Cog 1: PASM UART, Cogs 2..7: 6 independent MOS 6581 SID engines / 18 hardware voices with a 192-byte Hub RAM footprint).
* **[`SRR_HubMailbox.spin`](../examples/p1_spin/SRR_HubMailbox.spin):** Implements the Hub RAM mailbox, hardware lock arbitration (`lockset` / `lockclr`), 8-Cog routing, and request/reply buffer handoffs.
* **[`SRR_Demo.spin`](../examples/p1_spin/SRR_Demo.spin):** Multi-cog demo where Cog 0 (Client) issues synchronous `SendRequest` calls to Cog 1 (Server), receiving synchronous replies and emitting asynchronous pulses.

### 5.2 The Multi-SID Resource Manager: Stereo & 18-Voice Polyphony
SRR exposes up to 6 Propeller P1 MOS 6581 SID sound synthesizers (`service_id = 0x0050..0x0055` for Chips 0..5):

1. **Integrated Multi-Cog Simulation (`--sim`):**
   Test stereo chorus and 6-voice polyphony right from your desktop with zero hardware:
   ```bash
   make examples

   # Run Stereo Dual-SID Chorus Demo (Left on SID 0, Right on SID 1):
   ./examples/p1_sid/srr_sid_cli --sim stereo

   # Run 6-Voice Polyphonic Chord Synthesizer:
   ./examples/p1_sid/srr_sid_cli --sim poly
   ```
2. **Interactive Host Control over Physical UART:**
   Connect to a Propeller board (e.g. ActivityBoard, QuickStart) at 115,200 baud:
   ```bash
   # Silence all 6 SID chips (18 voices):
   ./examples/p1_sid/srr_sid_cli /dev/ttyUSB0 reset-all

   # Play Stereo Dual-SID Chorus Demo:
   ./examples/p1_sid/srr_sid_cli /dev/ttyUSB0 stereo

   # Play 6-Voice Polyphonic Chords:
   ./examples/p1_sid/srr_sid_cli /dev/ttyUSB0 poly

   # Target specific chip (e.g. Chip 1) and read registers:
   ./examples/p1_sid/srr_sid_cli /dev/ttyUSB0 --chip 1 read

   # Send asynchronous pulse to SID 1 Voice 1:
   ./examples/p1_sid/srr_sid_cli /dev/ttyUSB0 --chip 1 pulse 0x04 0x21
   ```

3. **Cross-Platform Python Client:**
   ```bash
   # Run Dual-SID stereo demo:
   python3 examples/p1_sid/srr_sid_client.py --port /dev/ttyUSB0 stereo
   ```

```spin
OBJ
  srr : "SRR_HubMailbox"

PUB Main | st, reply[16]
  LockId := locknew
  srr.Init(LockId)

  ' Send synchronous request to Cog 1 with 1-second timeout
  st := srr.SendRequest(1, 1, 1, string("PING"), 4, @reply, 16, clkfreq)
```

### 5.2 Lightweight P1 Emulators

#### `spinsim` (Zero-Dependency Command-Line Simulator)
`spinsim` is a famous, tiny single-file C program that simulates the 8-Cog Propeller 1 and executes Spin bytecodes and PASM binaries from the terminal in milliseconds.

```bash
# Build spinsim in 2 seconds:
curl -LO https://raw.githubusercontent.com/parallaxinc/spinsim/master/spinsim.c
gcc -O2 spinsim.c -o spinsim

# Compile Spin with openspin or flexspin:
openspin examples/p1_spin/SRR_Demo.spin -o SRR_Demo.binary

# Run in simulator:
./spinsim SRR_Demo.binary
```

---

## 6. Scripting Languages: Native FFI & Shared Libraries (`libsrr.so`)

While subprocess CLI execution (`srr_cli`) is ideal for shell scripts, high-throughput scripting applications (Python, Perl, PHP, Node.js, Ruby) can achieve **sub-microsecond in-process performance** by loading `libsrr.so` directly through Foreign Function Interfaces (FFI).

### 6.1 Building the Shared Library
Run:
```bash
make shared
```
This produces `libsrr.so` (Linux/macOS) or `libsrr_win32.dll` (Windows MinGW).

---

### 6.2 Python FFI (`ctypes`)

See the executable example in [`examples/scripting/srr_client_ffi.py`](../examples/scripting/srr_client_ffi.py):

```python
import ctypes

libsrr = ctypes.CDLL("./libsrr.so")

# Connect
conn = ctypes.c_void_p()
libsrr.srr_connect(b"srr_synth", None, ctypes.byref(conn))

# Synchronous Send / Receive
reply_buf = ctypes.create_string_buffer(4096)
actual_len = ctypes.c_size_t(0)

libsrr.srr_send_timed(conn, b"STATUS", 6, reply_buf, ctypes.sizeof(reply_buf), ctypes.byref(actual_len), 1000)
print("Reply:", reply_buf.raw[:actual_len.value].decode('utf-8'))

# Clean up
libsrr.srr_disconnect(conn)
```

---

### 6.3 PHP FFI (PHP 7.4+ Native)

PHP features built-in C FFI without external extensions:

```php
<?php
$ffi = FFI::cdef("
    typedef void srr_connection_t;
    int srr_connect(const char *name, void *opts, srr_connection_t **out);
    int srr_send_timed(srr_connection_t *conn, const void *data, size_t sz, void *rep, size_t cap, size_t *out_sz, uint32_t to);
    void srr_disconnect(srr_connection_t *conn);
", "./libsrr.so");

$conn = FFI::new("srr_connection_t*");
$ffi->srr_connect("srr_synth", null, FFI::addr($conn));

$reply = FFI::new("char[1024]");
$out_len = FFI::new("size_t");
$ffi->srr_send_timed($conn, "STATUS", 6, FFI::addr($reply), 1024, FFI::addr($out_len), 1000);

echo "PHP received: " . FFI::string($reply, $out_len->cdata) . "\n";
$ffi->srr_disconnect($conn);
```

---

### 6.4 Perl FFI (`FFI::Platypus`)

```perl
use FFI::Platypus 1.00;
my $ffi = FFI::Platypus->new(api => 1);
$ffi->lib('./libsrr.so');

$ffi->attach('srr_connect' => ['string', 'opaque', 'opaque*'] => 'int');
$ffi->attach('srr_send_timed' => ['opaque', 'string', 'size_t', 'string', 'size_t', 'size_t*', 'uint32'] => 'int');
$ffi->attach('srr_disconnect' => ['opaque'] => 'void');

my $conn;
$ffi->srr_connect("srr_synth", undef, \$conn);
```

---

### 6.5 Ruby FFI (`Fiddle` - Standard Library)

```ruby
require 'fiddle'
require 'fiddle/import'

module SRR
  extend Fiddle::Importer
  dlload './libsrr.so'
  extern 'int srr_connect(char*, void*, void**)'
  extern 'int srr_send_timed(void*, char*, size_t, char*, size_t, size_t*, uint32)'
  extern 'void srr_disconnect(void*)'
end
```

---

### 6.6 PowerShell (Windows P/Invoke)

On Windows using `libsrr_win32.dll`:

```powershell
$Signature = @"
[DllImport("libsrr_win32.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern int srr_connect(string name, IntPtr opts, out IntPtr conn);

[DllImport("libsrr_win32.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void srr_disconnect(IntPtr conn);
"@

$SRR = Add-Type -MemberDefinition $Signature -Name "NativeSRR" -Namespace "SRR" -PassThru
```
