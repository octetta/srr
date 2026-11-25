# Composing Distributed Systems with SRR: Topologies, Resolvers, and Federation

This guide explains how to compose complete, heterogeneous distributed systems using **SRR (Send / Receive / Reply)** as the unifying architectural backbone.

---

## 1. The Core Philosophy: Fractal Symmetry

Modern systems are often built by gluing together an incompatible patchwork of communication technologies:
* In-process threads use mutexes and lockless rings.
* Local processes use Unix Domain Sockets or HTTP/REST.
* Microcontrollers use ad-hoc binary serial protocols.
* Network nodes use gRPC, MQTT, or ZeroMQ.
* Browsers use WebSockets.

Every boundary switch requires translating data models, re-inventing timeouts, managing partial failures, and debugging disparate concurrency bugs.

**SRR replaces this fragmentation with a single, uniform interaction contract:**

```text
+-------------------------------------------------------------------------------+
|                       The Uniform SRR Contract                                |
|  - Synchronous Send / Receive / Reply for control and configuration           |
|  - Asynchronous One-Way Pulses for telemetry, alarms, and ticks               |
|  - Strict Single-Winner Lifecycle (timeouts, disconnects, replies)            |
+-------------------------------------------------------------------------------+
       |                      |                      |                     |
   In-Process               Host IPC             Serial Wire            Network
  (pthreads /             (Unix Sockets /       (UART / USB CDC /        (UDP /
   Propeller P1)          Windows Pipes)         Bare-Metal MCUs)       Wi-Fi / LAN)
```

Whether calling an audio DSP filter across threads, querying an Erlang service across IPC, adjusting a stepper motor on an ATmega328P over UART, or querying a fleet of ESP32 Wi-Fi pods over UDP, **the mental model and error semantics are identical**.

---

## 2. Architectural Building Blocks & Tiered Topologies

A robust SRR system is organized into five complementary tiers:

```text
+-------------------------------------------------------------------------------+
| Tier 4: Browser & UI Clients (HTML5 / WebAudio / WebSockets)                  |
+-------------------------------------------------------------------------------+
                                      |  RFC 6455 WebSocket Gateway
                                      v
+-------------------------------------------------------------------------------+
| Tier 1: Host IPC & Polyglot Services (Linux / macOS / Windows)                |
|  - Erlang / Elixir / Gleam Supervisors & State Machines (GenServer)           |
|  - High-Level Scripting & Orchestration (Python, Bash, Perl, Tcl)             |
|  - Service & Device Resolver Daemon (Directory & Routing Table)               |
+-------------------------------------------------------------------------------+
         |                                                       |
         | In-Process Pointers                                   | UDP Datagrams
         v                                                       v
+-----------------------------+               +---------------------------------+
| Tier 0: In-Process Real-Time|               | Tier 3: Network Microcontrollers|
|  - C Audio DSP Engines      |               |  - ESP32 Wi-Fi Pods (FreeRTOS)  |
|  - Propeller P1 Multi-Cog   |               |  - Pico W Distributed Nodes     |
|    Hub RAM Mailboxes        |               +---------------------------------+
+-----------------------------+                                  |
         |                                                       | Serial UART
         +--------------------------+----------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------------+
| Tier 2: Physical Microcontroller Peripherals (Zero-Heap Static Profiles)      |
|  - Raspberry Pi Pico (RP2040) Compact Profile (~2 KB RAM)                     |
|  - AVR ATmega328P / Arduino Tiny Profile (~392 Bytes RAM)                     |
+-------------------------------------------------------------------------------+
```

---

## 3. The Service & Device Resolver

In a dynamic or heterogeneous system, nodes and hardware peripherals shouldn't rely on brittle, hardcoded file paths (`/dev/ttyACM0`) or volatile IP addresses (`192.168.1.105:8888`).

The **SRR Resolver** acts as a decentralized directory. The Resolver is **itself an ordinary SRR service** that clients query using standard transactions:

### 3.1 Direct Registration vs. "On-Behalf-Of" Proxies

```text
1. Direct Self-Registration:
   [ESP32 Wi-Fi Node] --- (REGISTER "motor-1" UDP 192.168.1.42:8888 0 8193 30) ---> [Resolver]

2. On-Behalf-Of Proxy Registration:
   [Bare-Metal Arduino]
          | UART Handshake (device_info)
          v
   [Host USB Bridge]  --- (REGISTER "temp-sensor" SERIAL /dev/ttyACM0 115200 4097 0) -> [Resolver]
```

* **Microcontrollers remain simple:** The Arduino or AVR needs no IP stack, DHCP client, or discovery protocols. It runs pure C99 on static buffers.
* **Hotplug-friendly:** When a USB device is plugged in, a lightweight udev or CLI probe handshakes with the board and registers it with the Resolver.

---

## 4. Federated & Mesh Resolvers

Resolvers can cooperate over the local network using UDP, creating a unified distributed namespace without requiring centralized infrastructure (like Consul, etcd, or Avahi):

```text
  [Host A: Laptop / UI]                     [Host B: Robot Base / Controller]
  +--------------------+                    +--------------------+
  | Client Application |                    | Arduino Sensor Pod |
  +--------------------+                    +--------------------+
            |                                         | USB Serial
     RESOLVE "lidar"                                  |
            v                                         v
  +--------------------+    UDP Broadcast   +--------------------+
  | Local Resolver A   | =================> | Remote Resolver B  |
  |                    |    "RESOLVE lidar" |                    |
  +--------------------+ <================= +--------------------+
            |               FOUND lidar!              |
            |        (Rewritten as UDP Proxy)         |
            v                                         |
  Client talks directly to Host B via UDP             |
  ====================================================+
```

### The Three Federation Modes

1. **Subnet Mesh (mDNS / QNX Qnet style):**
   When Resolver A doesn't recognize a service name, it broadcasts `RESOLVE <name>` to `255.255.255.255:8889`. The peer that owns the service answers with its address and TTL. Resolver A caches the result.
2. **Upstream Forwarding (DNS style):**
   Edge devices forward unknown queries to a known master gateway resolver (`./tools/srr_resolver --upstream 192.168.1.1:8889`).
3. **Hardware Gateway Rewriting:**
   When Host B advertises a local serial peripheral (`/dev/ttyACM0`) to remote Host A, Host B's resolver rewrites the transport recipe to point to **Host B's network UDP gateway**. Host A never needs a physical serial cable—it accesses the remote MCU transparently over UDP.

---

## 5. Concrete Real-World System Architectures

### Case Study 1: Heterogeneous Modular Audio Workstation

```text
 +-----------------------------------------------------------------------+
 | Browser / Tablet UI (HTML5 / WebGL)                                   |
 | - Sliders, Oscilloscope, Preset Library                               |
 +-----------------------------------------------------------------------+
                                   | WebSocket (RFC 6455)
                                   v
 +-----------------------------------------------------------------------+
 | Gateway Daemon (gateway/srr_gateway.py)                               |
 | - Translates JSON WebSocket messages into binary SRR IPC frames       |
 +-----------------------------------------------------------------------+
                                   | Unix Domain Socket (/tmp/srr_synth.sock)
                                   v
 +-----------------------------------------------------------------------+
 | Synth Control Service (Erlang / Elixir OTP Supervision Tree)          |
 | - Voice allocation, envelope sequencing, MIDI file playback           |
 +-----------------------------------------------------------------------+
             |                                              |
             | In-Process Pointers                          | Serial UART
             v                                              v
+---------------------------------------+     +--------------------------+
| Native Audio DSP Engine (C99 pthread) |     | Hardware Control Box     |
| - 8 Bandlimited Polyphonic Voices     |     | - Raspberry Pi Pico      |
| - Lowpass Resonant Filter             |     | - 16 Physical Pot Knobs  |
| - Median latency: 6.7 µs              |     | - Sends Pulses on change |
+---------------------------------------+     +--------------------------+
```

#### End-to-End Interaction Flow:
1. **Physical Interaction:** The musician twists the physical filter resonance knob on the Pico control box.
2. **Hardware Pulse:** The Pico sends an asynchronous one-way pulse:
   ```c
   srr_endpoint_send_pulse(&ep, 1, OP_KNOB_CHANGE, &knob_payload, sizeof(knob_payload));
   ```
3. **OTP Coordination:** The Elixir service receives the pulse, validates bounds, updates voice parameters, and issues a synchronous transaction to the C DSP engine:
   ```elixir
   {:ok, "OK cutoff=2400.0"} = SRR.call(client, "SET cutoff 2400.0\0")
   ```
4. **UI Synchronization:** The Elixir service emits a pulse over the WebSocket gateway, updating the browser UI's slider without blocking the audio thread.

---

### Case Study 2: Distributed Autonomous Robotics Cell

```text
 +-----------------------------------------------------------------------+
 | Robot Brain / Mission Planner (Linux IPC, Python & C99)               |
 | - Path planning, SLAM, obstacle avoidance                             |
 +-----------------------------------------------------------------------+
             |                                              |
             | UDP Datagrams (Wi-Fi / Ethernet)             | USB Serial
             v                                              v
+---------------------------------------+     +--------------------------+
| Motor Pod 1 & 2 (ESP32 FreeRTOS)      |     | Safety Watchdog Node     |
| - Wheel encoders & PID speed control  |     | - AVR ATmega328P (Tiny)  |
| - Listens on UDP port 8888            |     | - Bumpers & E-Stop relay |
| - Zero-allocation FreeRTOS tasks      |     | - 392 Bytes static RAM   |
+---------------------------------------+     +--------------------------+
```

#### Why SRR Solves the Robotics Concurrency Problem:
* **Deterministic Rejection:** If Motor Pod 1's queue fills up because of an obstacle avoidance burst, SRR rejects new requests in **40 nanoseconds** (`SRR_QUEUE_FULL`), allowing the planner to instantly trigger safety evasions instead of suffering unbound queue bloat.
* **Single-Winner Timeout:** If the Wi-Fi link drops during a trajectory execution, the planner's `srr_send_timed()` deadline fires deterministically—it will never receive a stale phantom reply later.

---

### Case Study 3: High-Availability Industrial Process Controller

```text
  +--------------------------------+      +--------------------------------+
  | Supervisory Controller Node A  | <==> | Supervisory Controller Node B  |
  | (Primary Node over IPC / C99)  | UDP  | (Standby Node over IPC / C99)  |
  +--------------------------------+      +--------------------------------+
                   \                              /
                    \   Lock-Free Hub Mailbox    /
                     v                          v
  +------------------------------------------------------------------------+
  | Hard Real-Time I/O Subsystem (Parallax Propeller P1 Multi-Cog)         |
  | - 8 Independent 32-bit hardware RISC cores (Cogs)                      |
  | - Cog 0: Hub RAM arbiter and mailbox engine                            |
  | - Cog 1: 100 kHz valve PWM driver                                      |
  | - Cog 2: High-speed thermocouple ADC scanner                           |
  | - Cog 3: Dedicated hardware emergency scram monitor                    |
  +------------------------------------------------------------------------+
```

* **Hub RAM Arbitration:** Supervisory nodes communicate with the 8-Cog processor using the Stage 6 Hub RAM architecture ([`src/srr_p1.c`](file:///home/stewartj/book/srr/src/srr_p1.c)), achieving sub-microsecond register transfers without OS kernel overhead.
* **Peer Heartbeats:** Node A and Node B exchange periodic UDP pulses. If Node A misses 3 consecutive pulses, Node B claims the Hub mailbox lock and resumes supervision without interrupting the valve PWM Cog.

---

## 6. System Design Rules of Thumb

1. **Use Synchronous Requests for Control, Pulses for Events:**
   * If the caller must know the operation succeeded before continuing (e.g., `SET_VOLTAGE`, `ARM_MOTOR`), use `srr_send_timed()`.
   * If the event is telemetry, a sensor sample, or a periodic clock tick where blocking would stall the producer, use `srr_pulse()`.
2. **Always Bind Timeouts at the Edges:**
   * Never issue an unbounded `srr_send()` across network or serial boundaries. Always pass an explicit timeout (`srr_send_timed()` or `srr_udp_request(timeout_ms)`).
3. **Keep Microcontrollers Zero-Heap:**
   * Use caller-provided static buffers for `srr_endpoint_init()`. Embedded controllers must never call `malloc()` in real-time control loops.
4. **Leverage the Resolver for Environment Decoupling:**
   * Write client code that targets symbolic service names (`"synth"`, `"motor-left"`). Let the local Resolver resolve whether that service lives in a local thread, a Unix domain socket, a physical serial port, or a remote IP.
