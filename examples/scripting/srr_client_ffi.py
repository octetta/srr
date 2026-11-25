#!/usr/bin/env python3
"""
==============================================================================
SRR Python FFI Client (ctypes / libsrr.so)
==============================================================================
Demonstrates high-performance in-process C FFI from Python without CLI
subprocesses or custom socket byte-packing.
Loads `libsrr.so` (Linux) or `libsrr_win32.dll` (Windows) directly.
==============================================================================
"""

import ctypes
import os
import sys

# Locate libsrr.so relative to repo root
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SO_PATH = os.path.join(REPO_ROOT, "libsrr.so")

if not os.path.exists(SO_PATH):
    print(f"Error: {SO_PATH} not found. Please build it first with 'make shared'.", file=sys.stderr)
    sys.exit(1)

# Load shared library
libsrr = ctypes.CDLL(SO_PATH)

# Define C Status Enum
SRR_OK = 0
SRR_TIMEOUT = 1
SRR_CANCELLED = 2
SRR_DISCONNECTED = 3

# Define C function prototypes
# srr_status_t srr_connect(const char *service_name, const srr_connection_options_t *options, srr_connection_t **out_conn)
libsrr.srr_connect.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
libsrr.srr_connect.restype = ctypes.c_int

# srr_status_t srr_send_timed(srr_connection_t *conn, const void *send_data, size_t send_size,
#                              void *reply_buf, size_t reply_buf_size, size_t *out_reply_size, uint32_t timeout_ms)
libsrr.srr_send_timed.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_uint32
]
libsrr.srr_send_timed.restype = ctypes.c_int

# srr_status_t srr_pulse(srr_connection_t *conn, uint32_t code, uint64_t value)
libsrr.srr_pulse.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64]
libsrr.srr_pulse.restype = ctypes.c_int

# void srr_disconnect(srr_connection_t *conn)
libsrr.srr_disconnect.argtypes = [ctypes.c_void_p]
libsrr.srr_disconnect.restype = None

# const char* srr_status_str(srr_status_t status)
libsrr.srr_status_str.argtypes = [ctypes.c_int]
libsrr.srr_status_str.restype = ctypes.c_char_p


class SRRNativeClient:
    """Idiomatic Python context manager wrapping native libsrr.so via ctypes."""

    def __init__(self, service_name: str):
        self.service_name = service_name.encode('utf-8')
        self.conn = ctypes.c_void_p()

    def __enter__(self):
        st = libsrr.srr_connect(self.service_name, None, ctypes.byref(self.conn))
        if st != SRR_OK:
            err = libsrr.srr_status_str(st).decode('utf-8')
            raise ConnectionError(f"Failed to connect to SRR service '{self.service_name.decode()}': {err}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.conn:
            libsrr.srr_disconnect(self.conn)
            self.conn = None

    def send(self, payload: bytes, timeout_ms: int = 2000) -> bytes:
        reply_buf = ctypes.create_string_buffer(4096)
        actual_len = ctypes.c_size_t(0)

        st = libsrr.srr_send_timed(
            self.conn,
            payload,
            len(payload),
            reply_buf,
            ctypes.sizeof(reply_buf),
            ctypes.byref(actual_len),
            timeout_ms
        )
        if st != SRR_OK:
            err = libsrr.srr_status_str(st).decode('utf-8')
            raise RuntimeError(f"SRR transaction failed: {err}")

        return reply_buf.raw[:actual_len.value]

    def pulse(self, code: int, value: int):
        libsrr.srr_pulse(self.conn, code, value)


if __name__ == "__main__":
    print(f"Loaded {SO_PATH} successfully via Python ctypes.")
    print("SRR Native FFI Wrapper ready for use.")
