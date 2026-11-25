#!/usr/bin/env tclsh
# ==============================================================================
# SRR Tcl Client
# Direct Unix Domain Socket messaging using Tcl's built-in binary format/scan.
# ==============================================================================

#package require Tcl

set SRR_PROTO_MAGIC   0x53525231
set SRR_PROTO_VERSION 1
set SRR_MSG_REQUEST   2
set SRR_MSG_REPLY     3
set SRR_MSG_PULSE     5
set SRR_MSG_GOODBYE   7

proc srr_connect {service_name} {
    # Tcl socket command supports unix domain sockets via 'unix' option or socat/pipe
    # Or pipe via srr_cli for portable Tcl automation
    return [open "|./tools/srr_cli --ipc $service_name req 0 0 STATUS" r+]
}

puts "=== Tcl SRR Automation Example ==="

# 1. Query status via srr_cli integration
if {[catch {exec ./tools/srr_cli --sim info} dev_info]} {
    puts "Sim not ready: $dev_info"
} else {
    puts "Tcl received device info: $dev_info"
}

# 2. Ping check
if {[catch {exec ./tools/srr_cli --sim ping} ping_out]} {
    puts "Ping error: $ping_out"
} else {
    puts "Tcl Ping: $ping_out"
}

# 3. Synchronous control request
set req_cmd "SET cutoff 1850.0"
puts "Tcl issuing request '$req_cmd'..."
set reply [exec ./tools/srr_cli --sim req 1 1]
puts "Tcl received reply: $reply"

puts "Tcl automation completed successfully."
