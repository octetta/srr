#!/usr/bin/env bash
# ==============================================================================
# SRR Shell Automation Example
# Demonstrates using `srr_cli` from Bash scripts for deterministic control,
# health checking, parameter adjustment, and telemetry capture.
# ==============================================================================

set -euo pipefail

CLI="./tools/srr_cli"

# Ensure CLI and simulation binary exist
if [[ ! -x "$CLI" ]]; then
    echo "Error: $CLI not found. Please run 'make tools examples' first." >&2
    exit 1
fi

echo "================================================================="
echo " 1. Querying Microcontroller Telemetry via srr_cli (--sim)"
echo "================================================================="

# 1. Query device information into a shell variable
DEVICE_INFO=$("$CLI" --sim info)
echo "[INFO] Connected device: $DEVICE_INFO"

# 2. Ping device and capture response time
echo "[PING] Measuring round-trip ping latency..."
"$CLI" --sim ping

# 3. Read simulated sensor (ADC channel 1 -> Service 3, Op 1)
echo "[ADC] Reading sensor channel..."
ADC_REPLY=$("$CLI" --sim req 3 1)
echo "[ADC] Raw sensor output: $ADC_REPLY"

# 4. Write GPIO (Service 2, Op 2: Pin 25 -> HIGH)
echo "[GPIO] Setting onboard LED HIGH..."
GPIO_ACK=$("$CLI" --sim req 2 2 "LED_HIGH")
echo "[GPIO] Response: $GPIO_ACK"

# 5. Fire an asynchronous event pulse (Service 10, Op 1)
echo "[PULSE] Emitting one-way alarm notification pulse..."
"$CLI" --sim pulse 10 1 "ALARM_CLEAR"
echo "[PULSE] Emitted successfully."

echo ""
echo "================================================================="
echo " 2. Deterministic Error & Exit-Code Handling"
echo "================================================================="

# When an operation targets an unknown service, srr_cli exits with code 3 (Endpoint error)
set +e
"$CLI" --sim req 999 999 > /dev/null 2>&1
EXIT_CODE=$?
set -e

if [[ $EXIT_CODE -eq 3 ]]; then
    echo "[OK] Script correctly detected endpoint rejection (Exit Code $EXIT_CODE)."
else
    echo "[FAIL] Unexpected exit code: $EXIT_CODE" >&2
    exit 1
fi

echo ""
echo "Shell automation example completed successfully!"
