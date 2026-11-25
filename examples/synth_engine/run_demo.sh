#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="$DIR/synth_server"
CLI="$DIR/synth_cli"

echo "=== Starting Synthesizer Server in background ==="
"$SERVER" &
SERVER_PID=$!

# Ensure server cleanup on exit
trap 'kill -9 $SERVER_PID 2>/dev/null || true' EXIT

# Give server time to bind UDS socket
sleep 0.1

echo ""
echo "=== 1. Query initial engine status ==="
"$CLI" status

echo ""
echo "=== 2. Adjust synth parameters (Cutoff & Resonance) ==="
"$CLI" set cutoff 1850.5
"$CLI" set resonance 0.85
"$CLI" get cutoff

echo ""
echo "=== 3. Note Allocation & Voice Management ==="
"$CLI" note-on 60 127
"$CLI" note-on 64 110
"$CLI" note-on 67 95

echo ""
echo "=== 4. Send asynchronous clock pulses (one-way notifications) ==="
"$CLI" tick 16

echo ""
echo "=== 5. Query updated engine status ==="
"$CLI" status

echo ""
echo "=== 6. Release notes ==="
"$CLI" note-off 60
"$CLI" note-off 64

echo ""
echo "=== 7. Shut down server cleanly ==="
"$CLI" shutdown

# Wait for server process to exit
wait $SERVER_PID 2>/dev/null || true
trap - EXIT

echo ""
echo "=== Demo completed successfully! ==="
